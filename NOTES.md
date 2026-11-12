# 4.3.1 port notes

For the next time a Claws Mail upgrade breaks this. The README is the user page. This is the diary.

Test bed: Claws Mail 4.3.1, Debian 13. Plugin file: `claws-replyfromalias.so`. Working behavior is the 1.5.0-debug session, published as 1.0.0. Do not "simplify" the bits listed below without sending a reply to yourself and reading the received From.

The public name is **claws-replyfromalias**. The name Claws shows in the plugin list is **Reply From Alias**. Log, config, and the debug environment variable use the repo name:

| Thing | Path or string |
|---|---|
| Shared object | `claws-replyfromalias.so` |
| Config | `~/.claws-mail/claws-replyfromaliasrc` |
| Log | `~/.claws-mail/claws-replyfromalias.log` |
| One-session debug | `CLAWS_REPLYFROMALIAS_DEBUG=1` |
| Version line | `claws-replyfromalias 1.0.0 loaded` |

Internal C helpers (`load_aliases`, `apply_alias`, `find_alias`) kept their old names. They are not the plugin name.

## Landmines

1. Hook strings are the values, not the macro names, and not `*_hooklist`.

   ```
   src/compose.h
     COMPOSE_CREATED_HOOKLIST              "compose_created"
     COMPOSE_CHECK_BEFORE_SEND_HOOKLIST    "compose_check_before_send"
   ```

   `hooks_register_hook("compose_created_hooklist", ...)` returns id 1 and never runs. That was the "loaded, but reply adds nothing" bug. Confirm with a debug line `hook compose_created`, not with the id.

   ```bash
   grep -n 'define COMPOSE_.*HOOKLIST' src/compose.h
   grep -n 'hooks_invoke(COMPOSE_' src/compose.c
   ```

2. `generic_hook` returns FALSE. Always. `compose.c` treats TRUE from `compose_check_before_send` as "do not send".

3. Do not touch `account_combo`. Do not connect to its `changed` and also call `gtk_combo_box_set_active`. `set_active(-1)` emits `changed`, the handler refreshes, and the log fills with the same timestamp until you kill Claws. The visible From field is not the combo.

   What the combo actually is, on this build, so nobody has to rediscover it:

   ```
   GtkComboBox, has_entry=0, model=GtkListStore
   col 0 gchararray   visible "Name <email>", sometimes with &lt; &gt;
   col 1 gint
   col 2 gboolean
   renderer GtkCellRendererText
   ```

   No account pointer in the model. Patching row text did not survive, and the refresh loop ate the session. Leave it alone.

4. `compose->from_name` is a `GtkEntry`. Its text is `Name <email>`, not the name alone. That string is what made Send use the alias. Write the whole thing. Block the entry's `changed` handlers around `gtk_entry_set_text`, or Claws writes the account address back.

5. Compose fills that entry after `compose_created` returns. An idle is too early. 200ms (`FROM_SETTLE_MS`) was late enough on this machine. `compose_check_before_send` writes it once more, because Send builds the header from the field and will undo an early write. Keep both. The quiet log line is printed from the 200ms write, after checking the entry actually holds the alias.

6. `compose->account = copy` is a shallow `memcpy` of `PrefsAccount`. Replace `address` and `name` with your own `strdup`s. Free only those two. Everything else (SMTP host, user, password pointers) must stay shared with the real account. The copy is not in the account list. Do not `g_free` the struct's other members.

7. `procmsg_open_message(replyinfo, FALSE)` — second argument exists on 4.3.1. FALSE opens the message; we read until the header blank line and stop. Summary `To` is not enough for Bcc. `Delivered-To` is what matched in testing. Keep the header scan.

8. Undefined Claws symbols at link time are normal. They resolve when Claws `dlopen`s the plugin. Do not add `-Wl,--no-undefined`.

9. Quit Claws before replacing the `.so`. `pgrep -a claws`. A live process keeps the old mapping, the log keeps printing the old version, and you debug code that is not running.

## If the next version breaks

```bash
grep -n 'from_name\|account_combo\|replyinfo' src/compose.h
grep -n procmsg_open_message src/procmsg.h
grep -n 'define COMPOSE_.*HOOKLIST' src/compose.h
```

Silent on reply, version line present: hook renamed. Fix `HOOK_COMPOSE` / `HOOK_SEND` at the top of `claws-replyfromalias.c`.

Crash on reply: `Compose` or `PrefsAccount` layout changed. Rebuild against this tree first. If it still crashes, the field offsets moved and the shallow copy is reading off the struct. Do not ship that binary.

`no From field on this compose window`: `from_name` is no longer a `GtkEntry`, or the member moved. Grep the header. Do not guess a new widget and also poke the combo.

Version check: log line `claws-replyfromalias 1.0.0 loaded`. Bump `PLUGIN_VER` whenever the `.so` changes, or you cannot tell them apart.

## What was deliberately not done

- No preferences page. The combo work was enough GTK. The switch is the rc line and `CLAWS_REPLYFROMALIAS_DEBUG`.
- No account-combo row. It fought Claws and looped.
- No window-title suffix. Cosmetic, and it confused "did From change" with "did we set the title".
- Send forces the alias again even if the field was edited by hand, and even if the account combo was switched. That is the backstop that made Send correct. A later version can respect a hand edit only after checking a received message, not just the compose window.
- Forward / redirect were not tested. The handler bails out when `replyinfo` is NULL. If a forward sets `replyinfo`, From will change. Confirm before calling that supported.
- Header match does not dump header values. Do not add that "just for debugging" and then attach the log to a public issue. Addresses are already in it. Trim before posting.

## Report checklist

- Claws version (`claws-mail --version`) and whether it was distro or `/usr/local`.
- Log line with `claws-replyfromalias` version.
- With `debug` on: the `matched ... via ...` line, and whether `before-send` rewrote.
- Received From, not just the compose window.
- Not the whole log, and not a message that is still private. Strip ticket ids and subjects before posting.
