# naTE

**not another Terminal Emulator — tiling layouts, SSH, and serial support.**

[![CI](https://github.com/lhovind/naTE/actions/workflows/ci.yml/badge.svg)](https://github.com/lhovind/naTE/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20(experimental)-lightgrey)

<!-- screenshot -->

---

## What is naTE?

naTE (*not another Terminal Emulator*) is a graphical terminal emulator built for people who live in SSH sessions. It
gives you a tiling, tabbed interface where multiple terminals share a single window,
remembers your sessions across restarts so you can pick up exactly where you left
off, and ships with a full suite of SSH features — agent forwarding, X11 forwarding,
ProxyJump, keyboard-interactive (MFA) auth, and SCP file transfer — all without
touching the command line.

If you manage remote servers, work with serial consoles, or just want a terminal that
treats sessions as first-class objects rather than throwaway windows, naTE is built
for you.

---

## Features

### Terminal emulation
- VT100 / ANSI escape sequences, SGR attributes, OSC sequences
- UTF-8 text rendering (basic BMP characters; combining characters and wide/CJK characters not yet supported)
- Configurable scrollback buffer (default 100,000 lines)
- Alternate screen support (vim, htop, tmux) — also manually toggled via the alt-screen button in the tile title bar
- Bracketed paste with optional confirmation dialog
- URL detection and click-to-open

### Tiling & tabs
- Each window holds one or more **tiles** arranged in a grid; each tile has its own tab strip
- Open new tabs in the active tile via **Connection → New Connection in Tab**
- Move a session to its own tile in the same window with **Terminal → Move to New Tile**
- Move a session or tile to a separate window with **Terminal → Move to New Window**
- Open a blank second window from **Window → New Window**
- Drag a **tab** to reorder it within the same tile, move it to a different tile, or drop it onto another window
- Drag the **tile header** (blank area to the right of the `+` button) to move all sessions in that tile to another window
- Broadcast mode — send the same input to multiple sessions simultaneously

### Session management
- Auto-save and restore open sessions on launch
- Named session snapshots ("Save Session As…")
- Reconnect bar when a connection drops — resume without re-entering credentials

### SSH
- Authentication: password, public key, SSH agent, keyboard-interactive (MFA — Duo, YubiKey, PAM)
- SSH agent forwarding (`-A`)
- X11 forwarding (configured per connection profile; Xauthority cookie generated automatically)
- ProxyJump / bastion hop (single hop, auto-detected from `~/.ssh/config`)
- Auto-populate from `~/.ssh/config` — hosts, identity files, ProxyJump rules
- SSH agent identity hints for multi-key setups
- Keepalive and optional compression

### Serial
- Configurable baud rate, data bits, stop bits, parity, and flow control
- Optional dial script executed before I/O (for modem-style connections)

### File transfer
- SCP send and receive
- Remote directory browser for picking files

### Appearance
- Built-in themes: Solarized Dark, Solarized Light, xterm — all based on the [base16](https://github.com/chriskempson/base16) scheme
- Custom themes: drop a base16-compatible `.ini` file in `~/.nate/themes/`
- Font family and size picker (monospace fonts only)
- Cursor styles: Block, Bar, Underline — with optional blink
- Bell modes: None, Visual (screen flash), Audible
- Configurable cell padding

### Connection manager
- Save connection profiles (SSH, serial, local shell) with per-profile settings
- Word-wrap and column-width overrides per connection

---

## Installation

### Linux — AppImage (recommended)

Download the latest `naTE-x86_64.AppImage` from the
[Releases page](https://github.com/lhovind/naTE/releases), then:

```bash
chmod +x naTE-x86_64.AppImage
./naTE-x86_64.AppImage
```

No installation required. The AppImage bundles all dependencies.

### Build from source

#### Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| CMake | ≥ 3.20 | |
| Ninja | any | `ninja-build` on Debian/Ubuntu |
| GCC or Clang | C++20 | |
| GTK 3 dev headers | any | Linux only — `libgtk-3-dev` |
| OpenSSL dev headers | any | `libssl-dev` |
| pkg-config | any | Linux only |

wxWidgets, libssh2, and nlohmann/json are fetched automatically by CMake at
configure time — you do not need to install them separately.

#### Steps

```bash
git clone https://github.com/lhovind/naTE.git
cd naTE

cmake --preset release
cmake --build build-release --parallel $(nproc)

./build-release/naTE
```

**macOS note:** GTK 3 is not required on macOS; wxWidgets uses the native Cocoa
backend. macOS support is currently **experimental** — there is no CI coverage and
some features may behave differently.

---

## Quick Start

1. Launch naTE.
2. Open **File → New Connection** (or `Ctrl+N`).
3. Choose a transport tab: **SSH**, **Serial**, or **Local Shell**.
4. For SSH: enter the host, username, and authentication method, then click **Connect**.
5. To open more sessions, use **Connection → New Connection in Tab** to add a tab to
   the active tile, or **Terminal → Move to New Tile** to give a session its own tile
   in the same window. Open a second window any time with **Window → New Window**.
   You can also drag a tab between tiles or onto another window, and drag the blank
   area of a tile header to move the entire tile to a different window.
6. Save your layout for next time: **Connection → Save Session As…**

On the next launch, naTE restores your last session automatically. Named snapshots
let you keep multiple layouts and switch between them.

---

## Configuration

### config.ini

`config.ini` (in the same directory as the binary, or `~/.nate/config.ini`) controls
defaults for appearance, behavior, and terminal settings. Open
**Edit → Preferences** for a GUI over the most common options — changes take effect
immediately in open sessions.

### User data directory — `~/.nate/`

| Path | Contents |
|---|---|
| `~/.nate/connections.json` | Saved connection profiles |
| `~/.nate/snapshots.json` | Named session snapshots |
| `~/.nate/themes/` | Custom color themes |

### Custom themes

Themes are INI files following the [base16](https://github.com/chriskempson/base16) scheme, so any base16 theme can be adapted with minimal effort. Copy one of the built-in themes as a starting point:

```bash
cp themes/solarized-dark.ini ~/.nate/themes/my-theme.ini
# edit my-theme.ini, then select it in Edit → Preferences → Appearance
```

---

## Development

### Building for development

```bash
cmake --preset debug
cmake --build build --parallel $(nproc)
./build/naTE
```

The `debug` preset enables symbols and disables optimisations. The `release` preset
enables LTO.

### Running the test suite

```bash
cmake --preset debug
cmake --build build --target naTE_tests --parallel $(nproc)
ctest --preset debug
```

Tests are written with [Catch2](https://github.com/catchorg/Catch2). The suite
currently covers 246 scenarios across all major subsystems.

### Packaging (AppImage)

```bash
./scripts/build-appimage.sh
# produces naTE-x86_64.AppImage in the project root
```

### Architecture

naTE follows a ports-and-adapters layout. The core — terminal parsing (`src/parser/`),
document model (`src/document/`), session management (`src/session/`), and transport
backends (`src/transport/`) — is entirely headless and has no dependency on wxWidgets.
The wxWidgets UI (`src/ui/`) is one implementation of the presentation layer and is,
in principle, swappable for another frontend without touching the core. Persistence
uses a thin JSON repository layer (`src/db/`) so the storage format can change
without touching business logic.

---

## Roadmap

### Planned

- **SFTP subsystem** — replaces SCP; enables resume, directory operations, and
  compatibility with servers that disable SCP
- **Local port forwarding (`-L`)** — forward a local port through an SSH connection
- **Remote port forwarding (`-R`)** — expose a local port on the remote host
- **Double-click word delimiters** — configurable set of delimiter characters for
  word selection
- **Right-click action** — choose between paste and context menu on right-click


---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) *(coming soon)* for guidelines on submitting
issues and pull requests.


---

## License

GPL v3 — see [LICENSE](LICENSE).
