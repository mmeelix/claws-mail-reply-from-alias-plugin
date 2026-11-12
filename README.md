# claws-mail-reply-from-alias-plugin
This plugin makes replying to an email sent to your alias email address easy. A mailbox might carry emails from multiple alias email addresses. This plugin makes replying with the correct email address easy.

**Claws Mail plugin: reply From is the alias the message was sent to.**

You have one real mailbox and several addresses on it (`sales@…`, `jobs@…`, `hi@…`). Stock Claws Mail replies from the mailbox address. This plugin looks at who the original mail was addressed to, and sets From to that alias.

SMTP login stays the real mailbox. The mail server still has to allow that From (Gmail calls this **Send mail as**).

| | |
|---|---|
| Plugin name in Claws | Reply From Alias |
| File you load | `claws-replyfromalias.so` |
| Tested on | Claws Mail 4.3.1, Debian 13, reply only |
| License | [GPL-3.0-or-later](LICENSE) (same family as Claws Mail) |
| Config | `~/.claws-mail/claws-replyfromaliasrc` |
| Log | `~/.claws-mail/claws-replyfromalias.log` |

Claws Mail has no built-in alias list. The usual workaround is one extra account per alias. That works, and it stops being workable once you have several mailboxes and several aliases on each. This plugin keeps one account per real mailbox. Aliases live in a text file.

It does not add rows to the account dropdown. It does not change which account sends. It writes the From field.

---

## Who this is for

You already receive mail at more than one address in the same Claws account, and you want Reply to use the address the sender wrote to.

You do **not** need this if every address is already its own Claws account. Claws will reply from that account on its own.

---

## What you need before you start

1. **Claws Mail 4.x**, same major line as 4.3.1. This plugin was written and used against **4.3.1**.
2. A C compiler and the GTK 3 headers (`build-essential`, `pkg-config`, `libgtk-3-dev`).
3. The **Claws Mail source tree that matches the binary you run**, with `./configure` already done so `config.h` exists.
4. Permission on the server to send as each alias.

The plugin reads private Claws structs (`Compose`, `PrefsAccount`). A `.so` built against a different Claws than the one you run can crash on reply. Build against the source for *your* binary. There is no generic prebuilt `.so` in this repo for that reason.

---

## Install (Debian, Ubuntu, and close relatives)

### 1. Install build tools and get the Claws source

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev
sudo apt build-dep claws-mail
apt source claws-mail
```

`apt source` drops a directory such as `claws-mail-4.3.1` in the current folder. Use the version that matches `claws-mail --version`.

If your Claws came from source you built yourself, skip `apt source` and point `CLAWS_SRC` at that tree instead.

### 2. Configure that tree (headers only — you are not replacing Claws)

```bash
cd claws-mail-4.3.1
./configure
cd ..
```

`./configure` can take a few minutes. You need it so `config.h` and `claws-features.h` exist. You do not need to `make` or `make install` Claws itself.

### 3. Build the plugin

```bash
cd claws-replyfromalias
make CLAWS_SRC=/absolute/path/to/claws-mail-4.3.1
```

You should get `claws-replyfromalias.so` in this directory.

Undefined-symbol warnings at link time are normal. Those symbols resolve when Claws loads the plugin. Do not add `-Wl,--no-undefined`.

### 4. Quit Claws, then copy the plugin in

A running Claws keeps the old `.so` in memory. Quit it fully. `pgrep -a claws` should print nothing.

```bash
ls /usr/local/lib/claws-mail/plugins /usr/lib/claws-mail/plugins
sudo mkdir -p /usr/local/lib/claws-mail/plugins
sudo cp claws-replyfromalias.so /usr/local/lib/claws-mail/plugins/
```

Use the directory your Claws actually scans. Distro packages often use `/usr/lib/claws-mail/plugins`. A Claws you installed from source often uses `/usr/local/lib/claws-mail/plugins`.

### 5. Load it

Start Claws.

**Configuration → Plugins → Load…** and pick `claws-replyfromalias.so`, if it did not appear on its own.

Open `~/.claws-mail/claws-replyfromalias.log`. The first line must be:

```
claws-replyfromalias 1.0.0 loaded
```

If that line is missing, Claws was still running when you copied the file, or it loaded a different directory.

The first load also writes a commented sample config if `~/.claws-mail/claws-replyfromaliasrc` does not exist yet.

---

## Tell it your aliases

Edit `~/.claws-mail/claws-replyfromaliasrc`. One alias per line. Three columns, separated by `|`:

```
sales@example.com | me@example.com | Example Sales
jobs@example.com  | me@example.com | Example Jobs
hi@personal.net   | me@personal.net | Me
```

| Column | What to put |
|---|---|
| Alias | The address people write *to*. Case does not matter. |
| Mailbox address | The **Mail address** of the Claws account that receives it. Configuration → Edit accounts → the account → **Mail address**. Not the SMTP username, unless those happen to be the same string. |
| Display name | Optional. The name only, such as `Example Sales`. Leave it empty to keep the account's name. |

Blank lines and lines starting with `#` are ignored.

Save the file. You do not restart Claws. The next reply reads it again.

**Why the middle column matters.** It stops a reply you are writing from a different mailbox from being rewritten to an alias that belongs somewhere else. Set it.

**Plus-addresses are separate.** `sales+tickets@example.com` is not treated as `sales@example.com`. Add the plus-address as its own line if you want it.

A full commented copy of this format is in [`claws-replyfromaliasrc.example`](claws-replyfromaliasrc.example).

---

## Allow the alias on the mail server

The plugin only changes the From header Claws sends. The server can still replace it with the real mailbox address.

- **Gmail / Google Workspace:** Settings → Accounts → **Send mail as** → add each alias and confirm it.
- **Other providers:** look for "aliases", "identities", or "send as". If the provider forbids other From addresses, this plugin cannot override that.

---

## Check that it worked

1. Reply to a message that arrived at an alias. The From field should show `Example Sales <sales@example.com>`.
2. Send that reply to yourself. The message you receive must have From set to the alias. The mailbox account is still what authenticated.
3. Reply to a message that was sent only to the mailbox address. From stays the mailbox.
4. The log should grow by one line:

```
From me@example.com -> sales@example.com
```

Editing From by hand in that reply window is overwritten when you press Send. Close the window and start a new message (not a reply) if you want a different address this once. Switching the account dropdown in that same window is also overwritten on Send. That second write is what makes the sent message match the alias.

---

## If it does nothing
Open `~/.claws-mail/claws-replyfromalias.log`.

| What you see | What it means |
|---|---|
| Log has no `claws-replyfromalias 1.0.0 loaded` | Plugin is not loaded, or Claws was still running when you copied the `.so`. Quit, copy again, start. |
| Loaded line only, nothing on reply | This Claws build uses different hook names. See [NOTES.md](NOTES.md). A hook can "register" and still never run. |
| `skipped … alias belongs to` | Middle column does not match that account's Mail address. Copy it from Edit accounts. |
| Compose shows the alias, the mail you receive shows the mailbox | The server replaced From. Fix Send mail as. The plugin cannot. |
| `claws-replyfromaliasrc has no aliases` | Every line is still a comment, or a line was rejected (no `@`, or spaces inside the address). |
| Crash when you reply | The `.so` was built against a different Claws than the one running. Unload the plugin, rebuild against this binary's source, load it again. |

---

## Debug switch

Off unless you turn it on. Either of these enables a short trace:

Put this line in `claws-replyfromaliasrc` (picked up on the next reply, no restart):

```
debug
```

Or, for one session only:

```bash
CLAWS_REPLYFROMALIAS_DEBUG=1 claws-mail
```

`debug=off` turns the file switch off. If the environment variable is set to anything other than `0`, `off`, `no`, or `false`, debug stays on.

Quiet log is one line on load and one line per rewritten reply. Debug lines are marked with `D`. They name the header that matched (`Delivered-To`, `To`, `Received`, …). They do not dump header bodies.

The log still contains email addresses. Trim it before you paste it into a public issue. At 512 KB it rotates to `claws-replyfromalias.log.old`.

---

## After you upgrade Claws

1. Unload the plugin (**Configuration → Plugins**).
2. Upgrade Claws.
3. Get the new source tree, run `./configure` in it.
4. `make CLAWS_SRC=/path/to/new-tree` and copy the new `.so` in (Claws quit).
5. Load the plugin. Confirm the log shows `claws-replyfromalias 1.0.0 loaded`, then send one reply to yourself.

If reply crashes, or the log stays silent after the load line, the private structs or hook names moved. [NOTES.md](NOTES.md) has the greps and the list of things that already broke once.

---

## What it matches, in order

It opens the message you are replying to and scans headers. First hit wins. Inside one header, the order of lines in the config file wins.

1. `Delivered-To`, `Envelope-To`, `X-Original-To`, `X-Envelope-To`, `X-Delivered-To`, `X-Rcpt-To`, `Original-Recipient`
2. `To`, `Cc`, `Bcc`, `Apparently-To`, `Resent-To`
3. `Received` lines that say `for <alias>`
4. The summary To/Cc Claws already parsed (fallback)

`Delivered-To` is what matched in testing. The summary alone is not enough for Bcc.

Forward and redirect were not tested. If a forward has no reply target, the plugin leaves From alone.

---

## Files in this repo

| File | What it is |
|---|---|
| `claws-replyfromalias.c` | The plugin |
| `Makefile` | Build and install |
| `claws-replyfromaliasrc.example` | Config format, with comments |
| `NOTES.md` | Port notes for the next Claws upgrade |
| `LICENSE` | GPL-3.0-or-later |

---

## Keywords

Claws Mail plugin, reply from alias, reply from the address it was sent to, catch-all reply From, plus address, Send mail as, multiple addresses one mailbox, identity on reply.
