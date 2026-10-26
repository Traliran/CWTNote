# CWTNote

CWTNote is a local-first Markdown note application for the Linux desktop. It is
written in C with GTK 4 and libadwaita, keeps notes as plain `.md` files, and
records every save in a local Git repository. No account, cloud service, or
network connection is involved.

![CWTNote icon](data/icons/hicolor/scalable/apps/org.cwtnote.CWTNote.svg)

## Features

- Obsidian-style vault: folders and Markdown notes, no proprietary database.
- GtkSourceView editor with Markdown syntax highlighting.
- Live Markdown preview with headings, lists, quotes, code blocks, links and
  emphasis.
- `[[Wikilinks]]` with autocomplete while typing.
- Image and note embeds with `![[image.png]]` and `![[note.md]]`.
- Backlinks panel showing notes that link to or embed the current note.
- Search by note names and paths.
- Automatic local Git commit after every save, with per-note version history and
  one-click restore.
- "Revert Last Commit" action.
- Minimal quick-note window for `cwtnote --fast`, suitable for a global hotkey.
- Optional folder picker on first run; the default vault is `~/CWTNote`.

## Requirements

Runtime and build dependencies:

- GTK 4.12 or newer
- libadwaita 1.5 or newer
- GtkSourceView 5
- GLib / GIO
- `git` in `PATH`
- A C11 compiler, `make`, and `pkg-config`

On Fedora:

```sh
sudo dnf install gcc make pkg-config gtk4-devel libadwaita-devel \
    gtksourceview5-devel git
```

On Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-4-dev libadwaita-1-dev \
    libgtksourceview-5-dev git
```

On Arch Linux:

```sh
sudo pacman -S base-devel pkgconf gtk4 libadwaita gtksourceview5 git
```

## Build

```sh
make
```

Run from the build directory:

```sh
./cwtnote
```

Install system-wide (defaults to `/usr/local`):

```sh
sudo make install
```

Remove installed files:

```sh
sudo make uninstall
```

## First run

On the first launch CWTNote asks for a vault folder. The suggested path is
`~/CWTNote`; choose another folder with **Browse** if you prefer. The selected
path is stored in:

```
$XDG_CONFIG_HOME/cwtnote/config.conf
```

(usually `~/.config/cwtnote/config.conf`). On later launches the vault opens
automatically. The vault folder can be changed later from **Menu → Settings**.

If the folder does not exist, CWTNote creates it. A Git repository is
initialised inside the vault the first time it is opened.

## Writing notes

- Create notes and folders from the header buttons or the **Menu**.
- Right-click a note or folder in the sidebar for rename, delete, history, and
  creation actions.
- Type `[[` in a note to open the wikilink autocomplete. Use
  `[[Note name]]` or `[[Note name|display text]]`.
- Click a wikilink in the preview to open the target note.
- Put `![[image.png]]` on its own line to embed an image. Images are looked up
  next to the current note first and then anywhere in the vault.
- Put `![[Note.md]]` on its own line to embed another note. Its content is
  rendered with a quote-like left indent.
- Use **Ctrl+S** to save immediately. Changes are also saved automatically
  shortly after you stop typing.
- Toggle the preview with the eye button in the header.

Notes deleted from CWTNote are moved to the system trash. Renaming a folder
does not rewrite wikilinks inside other notes.

## Local Git history

CWTNote calls the real `git` binary and never configures a remote. It does not
push or pull.

- Every save creates a commit containing all pending changes in the vault.
- A commit message is built from the first 15 UTF-8 characters of the added
  text, followed by `...`. When there is no added text, `Update notes...` is
  used.
- If the vault is not a repository yet, `git init` runs automatically.
- If `user.name` or `user.email` is missing, a repository-local identity of
  `CWTNote <cwtnote@localhost>` is used. Global Git settings are not modified.
- Automatic commits disable GPG signing (`commit.gpgsign=false`) so that a
  signing prompt cannot block a save. Your global Git configuration is left
  untouched.
- **Menu → History** lists the commits that touched the current note and lets
  you restore any version. Restoring creates a new commit.
- **Menu → Revert Last Commit** creates a new commit that undoes the most recent
  change in the repository.

## Quick notes

```sh
cwtnote --fast
```

This opens a small floating window with a single text area and no sidebar:

- The text area is focused automatically.
- **Ctrl+Enter** saves the note, commits it, and closes the window.
- **Escape** closes the window without saving.
- Empty notes are discarded.

Quick notes are written to `Inbox/YYYY-MM-DD-HHMMSS.md` inside the same vault.
The command works whether the main window is open or not. If CWTNote is already
running, the command is forwarded to the running instance.

### Binding a global hotkey

Use `cwtnote --fast` as the command in your desktop's keyboard settings.

GNOME (Settings → Keyboard → Custom Shortcuts):

```
Name:    CWTNote quick note
Command: cwtnote --fast
```

KDE Plasma (System Settings → Shortcuts → Custom Shortcuts): add a new command
shortcut with the same command.

Hyprland (`~/.config/hypr/hyprland.conf`):

```ini
bind = SUPER, N, exec, cwtnote --fast
```

Sway (`~/.config/sway/config`):

```
bindsym $mod+n exec cwtnote --fast
```

Make sure `cwtnote` is in `PATH` (for example after `sudo make install`).

## Project layout

```
src/app.c        Main window, sidebar, editor, history and settings
src/app.h        Shared application state
src/git.c        Local Git wrapper (init, commit, log, show, revert)
src/git.h
src/main.c       Application entry point and --fast option
src/markdown.c   Markdown preview renderer
src/markdown.h
src/quick.c      Quick-note window
src/util.c       Config, paths and UTF-8 helpers
src/util.h
src/vault.c      Notes, folders, backlinks and link resolution
src/vault.h
data/            Desktop entry and application icon
```

## License

CWTNote is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

CWTNote is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the `LICENSE` file for the full text.
