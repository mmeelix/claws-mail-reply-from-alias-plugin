/*
 * claws-replyfromalias 1.0.0 — on reply, From is the alias the message
 * was sent to.
 *
 * Tested against Claws Mail 4.3.1. Build against that same source tree.
 * Compose and PrefsAccount are private structs; a mismatch crashes.
 *
 * Landmines, both hit while bringing this up:
 *   - Register "compose_created", not "compose_created_hooklist".
 *     hooks_register_hook() returns id 1 even for a name nothing invokes.
 *   - Do not touch account_combo, and do not return TRUE from a hook.
 *     TRUE from compose_check_before_send cancels the send.
 *
 * Build, from a tree where ./configure has already been run:
 *   gcc -shared -fPIC -o claws-replyfromalias.so claws-replyfromalias.c \
 *     -DHAVE_CONFIG_H \
 *     -I/path/to/claws-mail-4.3.1 \
 *     -I/path/to/claws-mail-4.3.1/src \
 *     -I/path/to/claws-mail-4.3.1/src/common \
 *     -I/path/to/claws-mail-4.3.1/src/gtk \
 *     $(pkg-config --cflags gtk+-3.0 glib-2.0)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef HAVE_CONFIG_H
#  include "config.h"
#  include "claws-features.h"
#endif

#include <glib.h>
#include <gtk/gtk.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#include "defs.h"
#include "plugin.h"
#include "version.h"
#include "hooks.h"
#include "prefs_account.h"
#include "compose.h"
#include "procmsg.h"
#include "utils.h"

#define PLUGIN_VER       "1.0.0"
#define FROM_SETTLE_MS   200
#define LOG_MAX          (512 * 1024)
#define HEADER_MAX       (64 * 1024)

/* Strings compose.c actually passes to hooks_invoke(). Not the macro names. */
static const char *const HOOK_COMPOSE = "compose_created";
static const char *const HOOK_SEND    = "compose_check_before_send";

static const char *const hook_names[] = {
    HOOK_COMPOSE,
    HOOK_SEND,
    NULL
};
static guint hook_ids[4];

static GSList *aliases;
static GSList *overrides;
static GSList *pending_laters;
static gboolean debug_on;
static gboolean aliases_loaded;
static time_t aliases_mtime;

typedef struct {
    gchar *alias;
    gchar *parent;
    gchar *name;
} Alias;

typedef struct {
    GtkWidget *window;
    Compose *compose;
    PrefsAccount *orig;
    PrefsAccount *copy; /* shallow copy; we own address and name only */
    gulong destroy_id;
    guint timeout_id;
} Override;

typedef struct {
    Compose *compose;
    gint tries;
    guint idle_id;
} Later;

static void alias_free(gpointer p)
{
    Alias *a = p;
    g_free(a->alias);
    g_free(a->parent);
    g_free(a->name);
    g_free(a);
}

static void logv(gboolean dbg, const char *fmt, va_list ap)
{
    const gchar *rc = get_rc_dir();
    gchar *path, *old;
    FILE *fp;
    struct stat st;
    time_t t = time(NULL);
    struct tm tm;
    char tbuf[32];

    if (!rc)
        return;
    path = g_build_filename(rc, "claws-replyfromalias.log", NULL);
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX) {
        old = g_strconcat(path, ".old", NULL);
        rename(path, old);
        g_free(old);
    }
    fp = fopen(path, "a");
    g_free(path);
    if (!fp)
        return;
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof tbuf, "%F %T", &tm);
    fprintf(fp, "%s%s ", tbuf, dbg ? " D" : "");
    vfprintf(fp, fmt, ap);
    fputc('\n', fp);
    fflush(fp);
    fclose(fp);
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv(FALSE, fmt, ap);
    va_end(ap);
}

static void dlog(const char *fmt, ...)
{
    va_list ap;
    if (!debug_on)
        return;
    va_start(ap, fmt);
    logv(TRUE, fmt, ap);
    va_end(ap);
}

static gboolean env_debug(void)
{
    const char *e = g_getenv("CLAWS_REPLYFROMALIAS_DEBUG");
    if (!e || !e[0] || e[0] == '0')
        return FALSE;
    if (!g_ascii_strcasecmp(e, "off") || !g_ascii_strcasecmp(e, "no")
        || !g_ascii_strcasecmp(e, "false"))
        return FALSE;
    return TRUE;
}

static gboolean bad_addr(const char *s)
{
    return !s || !s[0] || strlen(s) > 320 || !strchr(s, '@')
        || strpbrk(s, " \t\r\n<>");
}

static gchar *clean_name(const char *s)
{
    GString *b;
    const char *p;
    gsize i;

    if (!s || !s[0])
        return NULL;
    b = g_string_new(NULL);
    for (p = s; *p && b->len < 200; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f || c == '<' || c == '>')
            continue;
        g_string_append_c(b, *p);
    }
    while (b->len && (b->str[0] == ' ' || b->str[0] == '\t'))
        g_string_erase(b, 0, 1);
    i = b->len;
    while (i && (b->str[i - 1] == ' ' || b->str[i - 1] == '\t'))
        i--;
    g_string_truncate(b, i);
    if (b->len == 0) {
        g_string_free(b, TRUE);
        return NULL;
    }
    return g_string_free(b, FALSE);
}

/* "debug", "debug=on", "debug off". Returns TRUE if the line is the switch. */
static gboolean debug_directive(const char *line, gboolean *on)
{
    const char *v;

    if (!g_ascii_strcasecmp(line, "debug")) {
        *on = TRUE;
        return TRUE;
    }
    if (g_ascii_strncasecmp(line, "debug=", 6)
        && g_ascii_strncasecmp(line, "debug ", 6))
        return FALSE;
    v = line + 6;
    while (*v == ' ' || *v == '\t' || *v == '=')
        v++;
    if (*v == '\0' || !g_ascii_strcasecmp(v, "1") || !g_ascii_strcasecmp(v, "on")
        || !g_ascii_strcasecmp(v, "yes") || !g_ascii_strcasecmp(v, "true"))
        *on = TRUE;
    else
        *on = FALSE;
    return TRUE;
}

static void load_aliases(void)
{
    gchar *path, *data = NULL;
    gchar **lines;
    struct stat st;
    gboolean file_set = FALSE, file_on = FALSE;
    guint i, n = 0;

    path = g_build_filename(get_rc_dir(), "claws-replyfromaliasrc", NULL);
    if (stat(path, &st) == 0 && aliases_loaded && st.st_mtime == aliases_mtime) {
        g_free(path);
        return;
    }

    g_slist_free_full(aliases, alias_free);
    aliases = NULL;
    aliases_loaded = FALSE;

    if (!g_file_get_contents(path, &data, NULL, NULL)) {
        info("claws-replyfromaliasrc missing: %s", path);
        aliases_mtime = (time_t)-1;
        g_free(path);
        debug_on = env_debug();
        return;
    }

    lines = g_strsplit(data, "\n", -1);
    g_free(data);
    for (i = 0; lines[i]; i++) {
        gchar *line = lines[i];
        gchar **parts;
        Alias *a;
        gboolean on = FALSE;

        if (i == 0 && (unsigned char)line[0] == 0xef
            && (unsigned char)line[1] == 0xbb
            && (unsigned char)line[2] == 0xbf)
            line += 3;
        g_strstrip(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (debug_directive(line, &on)) {
            file_set = TRUE;
            file_on = on;
            continue;
        }
        parts = g_strsplit(line, "|", 3);
        g_strstrip(parts[0]);
        if (bad_addr(parts[0])) {
            dlog("line %u rejected", i + 1);
            g_strfreev(parts);
            continue;
        }
        a = g_new0(Alias, 1);
        a->alias = g_strdup(parts[0]);
        if (parts[1]) {
            g_strstrip(parts[1]);
            if (parts[1][0] && !bad_addr(parts[1]))
                a->parent = g_strdup(parts[1]);
        }
        if (parts[2]) {
            g_strstrip(parts[2]);
            a->name = clean_name(parts[2]);
        }
        if (a->parent && !g_ascii_strcasecmp(a->alias, a->parent)) {
            dlog("line %u: alias equals mailbox, skipped", i + 1);
            alias_free(a);
            g_strfreev(parts);
            continue;
        }
        aliases = g_slist_append(aliases, a);
        n++;
        g_strfreev(parts);
    }
    g_strfreev(lines);

    aliases_loaded = TRUE;
    aliases_mtime = (stat(path, &st) == 0) ? st.st_mtime : (time_t)-1;
    g_free(path);

    debug_on = env_debug() || (file_set && file_on);
    if (n == 0)
        info("claws-replyfromaliasrc has no aliases");
    else
        dlog("%u aliases", n);
}

static void ensure_sample_config(void)
{
    gchar *path = g_build_filename(get_rc_dir(), "claws-replyfromaliasrc", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        const char *sample =
            "# alias | mailbox address | display name\n"
            "# uncomment the next line for match details in claws-replyfromalias.log\n"
            "# debug\n"
            "#\n"
            "# sales@example.com | me@example.com | Example Sales\n";
        g_file_set_contents(path, sample, -1, NULL);
        info("wrote %s", path);
    }
    g_free(path);
}

static const char *ascii_strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!hay || n == 0)
        return hay;
    for (; *hay; hay++) {
        if (g_ascii_strncasecmp(hay, needle, n) == 0)
            return hay;
    }
    return NULL;
}

static gboolean is_addr_char(char c)
{
    return g_ascii_isalnum(c) || c == '.' || c == '_' || c == '%'
        || c == '+' || c == '-' || c == '=';
}

static gboolean addr_in_text(const char *text, const char *addr)
{
    const char *p;
    size_t n;
    if (!text || !addr || !*addr)
        return FALSE;
    n = strlen(addr);
    for (p = text; (p = ascii_strcasestr(p, addr)); p += n) {
        char left = (p == text) ? ' ' : p[-1];
        char right = p[n];
        if (!is_addr_char(left) && !is_addr_char(right))
            return TRUE;
    }
    return FALSE;
}

static gboolean scan_header(const char *raw, const char *name,
                            gboolean (*pred)(const char *value, gpointer u),
                            gpointer u)
{
    size_t nlen = strlen(name);
    const char *p = raw;
    GString *val = NULL;
    gboolean in = FALSE;
    gboolean found = FALSE;

    if (!raw)
        return FALSE;
    while (*p && !found) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = g_strndup(p, len);

        g_strchomp(line);
        if (line[0] == ' ' || line[0] == '\t') {
            if (in && val) {
                g_string_append_c(val, ' ');
                g_string_append(val, g_strstrip(line));
            }
        } else {
            if (in && val) {
                found = pred(val->str, u);
                g_string_free(val, TRUE);
                val = NULL;
                in = FALSE;
            }
            if (!found && g_ascii_strncasecmp(line, name, nlen) == 0
                && line[nlen] == ':') {
                char *v = line + nlen + 1;
                while (*v == ' ' || *v == '\t')
                    v++;
                val = g_string_new(v);
                in = TRUE;
            }
        }
        g_free(line);
        if (!eol)
            break;
        p = eol + 1;
    }
    if (in && val && !found)
        found = pred(val->str, u);
    if (val)
        g_string_free(val, TRUE);
    return found;
}

static gboolean pred_addr(const char *value, gpointer u)
{
    return addr_in_text(value, u);
}

static gboolean pred_received(const char *value, gpointer u)
{
    const char *alias = u;
    char *p1 = g_strdup_printf("for <%s>", alias);
    gboolean ok = ascii_strcasestr(value, p1) != NULL;
    g_free(p1);
    if (ok)
        return TRUE;
    {
        const char *hit = value;
        size_t n = strlen(alias);
        while ((hit = ascii_strcasestr(hit, "for "))) {
            const char *a = hit + 4;
            if (g_ascii_strncasecmp(a, alias, n) == 0 && !is_addr_char(a[n]))
                return TRUE;
            hit += 4;
        }
    }
    return FALSE;
}

/*
 * Header priority, then file order within a header.
 * Delivered-To beats To. Summary To/Cc is only a fallback.
 */
static Alias *find_alias(const char *raw, const char *sum_to, const char *sum_cc,
                         const char **how)
{
    static const char *names[] = {
        "Delivered-To", "Envelope-To", "X-Original-To", "X-Envelope-To",
        "X-Delivered-To", "X-Rcpt-To", "Original-Recipient",
        "To", "Cc", "Bcc", "Apparently-To", "Resent-To", NULL
    };
    int i;

    *how = NULL;
    for (i = 0; names[i]; i++) {
        GSList *l;
        for (l = aliases; l; l = l->next) {
            Alias *a = l->data;
            if (scan_header(raw, names[i], pred_addr, a->alias)) {
                *how = names[i];
                return a;
            }
        }
    }
    if (raw) {
        GSList *l;
        for (l = aliases; l; l = l->next) {
            Alias *a = l->data;
            if (scan_header(raw, "Received", pred_received, a->alias)) {
                *how = "Received";
                return a;
            }
        }
    }
    {
        GSList *l;
        for (l = aliases; l; l = l->next) {
            Alias *a = l->data;
            if (addr_in_text(sum_to, a->alias) || addr_in_text(sum_cc, a->alias)) {
                *how = "summary To/Cc";
                return a;
            }
        }
    }
    return NULL;
}

static char *read_headers_fp(FILE *fp)
{
    GString *s = g_string_new(NULL);
    char buf[8192];

    while (s->len < HEADER_MAX && fgets(buf, sizeof buf, fp)) {
        g_string_append(s, buf);
        if (buf[0] == '\n' || (buf[0] == '\r' && buf[1] == '\n'))
            break;
    }
    return g_string_free(s, FALSE);
}

static Override *find_override(GtkWidget *w)
{
    GSList *l;
    for (l = overrides; l; l = l->next) {
        Override *o = l->data;
        if (o->window == w)
            return o;
    }
    return NULL;
}

static gboolean field_has(Compose *c, const char *addr)
{
    if (!c || !addr || !c->from_name || !GTK_IS_ENTRY(c->from_name))
        return FALSE;
    return addr_in_text(gtk_entry_get_text(GTK_ENTRY(c->from_name)), addr);
}

/* Entry text is "Name <addr>", not the name alone. Block changed so Claws
 * does not immediately write the account address back. */
static void set_from_field(Compose *c, const char *name, const char *addr)
{
    GtkEntry *entry;
    gchar *text;
    guint sig;

    if (!c || !addr)
        return;
    if (!c->from_name || !GTK_IS_ENTRY(c->from_name)) {
        info("no From field on this compose window");
        return;
    }
    entry = GTK_ENTRY(c->from_name);
    text = (name && name[0])
        ? g_strdup_printf("%s <%s>", name, addr)
        : g_strdup(addr);
    dlog("From field \"%s\" -> \"%s\"", gtk_entry_get_text(entry), text);
    sig = g_signal_lookup("changed", GTK_TYPE_ENTRY);
    if (sig)
        g_signal_handlers_block_matched(entry, G_SIGNAL_MATCH_ID,
                                        sig, 0, NULL, NULL, NULL);
    gtk_entry_set_text(entry, text);
    if (sig)
        g_signal_handlers_unblock_matched(entry, G_SIGNAL_MATCH_ID,
                                          sig, 0, NULL, NULL, NULL);
    g_free(text);
}

static void apply_from(Override *o)
{
    if (!o || !o->compose || !o->copy || !o->copy->address)
        return;
    if (!o->window || !GTK_IS_WIDGET(o->window))
        return;
    o->compose->account = o->copy;
    set_from_field(o->compose, o->copy->name, o->copy->address);
}

static void override_free(Override *o, gboolean restore_ptr)
{
    if (!o)
        return;
    overrides = g_slist_remove(overrides, o);
    if (o->timeout_id) {
        g_source_remove(o->timeout_id);
        o->timeout_id = 0;
    }
    if (o->destroy_id && o->window && GTK_IS_WIDGET(o->window))
        g_signal_handler_disconnect(o->window, o->destroy_id);
    if (restore_ptr && o->compose && o->window && GTK_IS_WIDGET(o->window)
        && o->compose->account == o->copy)
        o->compose->account = o->orig;
    if (o->copy) {
        g_free(o->copy->address);
        g_free(o->copy->name);
        g_free(o->copy);
    }
    g_free(o);
}

static void on_window_destroy(GtkWidget *w, gpointer data)
{
    Override *o = data;
    (void)w;
    o->destroy_id = 0;
    override_free(o, FALSE);
}

static gboolean from_later(gpointer data)
{
    Override *o = data;

    o->timeout_id = 0;
    apply_from(o);
    if (o->compose && field_has(o->compose, o->copy->address))
        info("From %s -> %s", o->orig->address, o->copy->address);
    else
        info("From field did not stick (%s)", o->copy->address);
    return FALSE;
}

static void apply_alias(Compose *compose, Alias *match, const char *how)
{
    PrefsAccount *orig = compose->account;
    PrefsAccount *copy;
    Override *o;

    dlog("matched %s via %s", match->alias, how ? how : "?");
    if (!orig || !orig->address || !compose->window)
        return;
    if (find_override(compose->window))
        return;
    if (match->parent
        && g_ascii_strcasecmp(match->parent, orig->address) != 0) {
        info("skipped %s: account is %s, alias belongs to %s",
             match->alias, orig->address, match->parent);
        return;
    }

    /* Share every pointer with the real account except address and name.
     * SMTP settings stay the mailbox. Do not free anything else on copy. */
    copy = g_new0(PrefsAccount, 1);
    memcpy(copy, orig, sizeof(PrefsAccount));
    copy->address = g_strdup(match->alias);
    if (match->name && match->name[0])
        copy->name = g_strdup(match->name);
    else if (orig->name && orig->name[0])
        copy->name = g_strdup(orig->name);
    else
        copy->name = NULL;

    o = g_new0(Override, 1);
    o->window = compose->window;
    o->compose = compose;
    o->orig = orig;
    o->copy = copy;
    overrides = g_slist_prepend(overrides, o);
    o->destroy_id = g_signal_connect_after(compose->window, "destroy",
                                           G_CALLBACK(on_window_destroy), o);
    apply_from(o);
    o->timeout_id = g_timeout_add(FROM_SETTLE_MS, from_later, o);
}

static void handle_reply(Compose *compose)
{
    FILE *fp;
    char *headers = NULL;
    const char *how = NULL;
    Alias *match;

    load_aliases();
    if (!aliases || !compose->replyinfo)
        return;

    /* 4.3.1: procmsg_open_message(MsgInfo *, gboolean). FALSE = whole message.
     * We stop at the header blank line. */
    fp = procmsg_open_message(compose->replyinfo, FALSE);
    if (fp) {
        headers = read_headers_fp(fp);
        fclose(fp);
        dlog("original headers %u bytes", headers ? (unsigned)strlen(headers) : 0);
    } else {
        dlog("could not open original, summary To/Cc only");
    }

    match = find_alias(headers, compose->replyinfo->to, compose->replyinfo->cc, &how);
    g_free(headers);
    if (!match) {
        dlog("no alias matched");
        return;
    }
    apply_alias(compose, match, how);
}

static void later_unlink(Later *L)
{
    L->idle_id = 0;
    pending_laters = g_slist_remove(pending_laters, L);
    g_free(L);
}

static gboolean later_cb(gpointer data)
{
    Later *L = data;
    Compose *c = L->compose;

    if (!c || !c->window || !GTK_IS_WIDGET(c->window)) {
        later_unlink(L);
        return FALSE;
    }
    if (!c->replyinfo) {
        if (++L->tries < 8)
            return TRUE;
        dlog("compose has no replyinfo");
        later_unlink(L);
        return FALSE;
    }
    handle_reply(c);
    later_unlink(L);
    return FALSE;
}

static void before_send(Compose *c)
{
    Override *o;
    gboolean already;

    if (!c || !c->window)
        return;
    o = find_override(c->window);
    if (!o || !o->copy)
        return;
    already = (c->account == o->copy && field_has(c, o->copy->address));
    apply_from(o);
    if (already)
        dlog("before-send already %s", o->copy->address);
    else if (field_has(c, o->copy->address))
        info("before-send rewrote From to %s", o->copy->address);
    else
        info("before-send failed to set %s", o->copy->address);
}

/* Must return FALSE. TRUE stops later plugins and, on the send hook, aborts send. */
static gboolean generic_hook(gpointer source, gpointer data)
{
    const char *name = data;

    dlog("hook %s", name ? name : "?");
    if (name && strcmp(name, HOOK_COMPOSE) == 0 && source) {
        Later *L = g_new0(Later, 1);
        L->compose = source;
        pending_laters = g_slist_prepend(pending_laters, L);
        L->idle_id = g_idle_add(later_cb, L);
        if (!L->idle_id)
            later_unlink(L);
    } else if (name && strcmp(name, HOOK_SEND) == 0 && source) {
        before_send(source);
    }
    return FALSE;
}

static void drop_laters(void)
{
    while (pending_laters) {
        Later *L = pending_laters->data;
        guint id = L->idle_id;
        pending_laters = g_slist_delete_link(pending_laters, pending_laters);
        L->idle_id = 0;
        if (id)
            g_source_remove(id);
        g_free(L);
    }
}

struct PluginFeature *plugin_provides(void)
{
    static struct PluginFeature features[] = {
        { PLUGIN_UTILITY, "Reply From Alias" },
        { PLUGIN_NOTHING, NULL }
    };
    return features;
}

gint plugin_init(gchar **error)
{
    int i;

    if (!check_plugin_version(MAKE_NUMERIC_VERSION(4, 0, 0, 0),
                              VERSION_NUMERIC, "Reply From Alias", error))
        return -1;

    ensure_sample_config();
    load_aliases();
    info("claws-replyfromalias %s loaded rc=%s%s",
         PLUGIN_VER, get_rc_dir() ? get_rc_dir() : "?",
         debug_on ? " debug=on" : "");

    for (i = 0; hook_names[i]; i++) {
        hook_ids[i] = hooks_register_hook(hook_names[i], generic_hook,
                                          (gpointer)hook_names[i]);
        if (!hook_ids[i])
            info("failed to register %s", hook_names[i]);
        else
            dlog("registered %s id=%u", hook_names[i], hook_ids[i]);
    }
    return 0;
}

gboolean plugin_done(void)
{
    int i;

    for (i = 0; hook_names[i]; i++) {
        if (hook_ids[i])
            hooks_unregister_hook(hook_names[i], hook_ids[i]);
        hook_ids[i] = 0;
    }
    drop_laters();
    while (overrides)
        override_free(overrides->data, TRUE);
    g_slist_free_full(aliases, alias_free);
    aliases = NULL;
    aliases_loaded = FALSE;
    info("claws-replyfromalias %s unloaded", PLUGIN_VER);
    return TRUE;
}

const gchar *plugin_name(void)    { return "Reply From Alias"; }
const gchar *plugin_desc(void)
{
    return
        "On reply, set From to the alias the message was sent to.\n"
        "\n"
        "Config: ~/.claws-mail/claws-replyfromaliasrc\n"
        "Log: ~/.claws-mail/claws-replyfromalias.log\n"
        "Debug switch: a line \"debug\" in that file, or CLAWS_REPLYFROMALIAS_DEBUG=1.\n"
        "\n"
        "SMTP still uses the mailbox account. The server must accept the alias as From.";
}
const gchar *plugin_type(void)    { return "GTK3"; }
const gchar *plugin_licence(void) { return "GPL3+"; }
const gchar *plugin_version(void) { return PLUGIN_VER; }
