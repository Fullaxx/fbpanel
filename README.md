# fbpanel

A lightweight GTK2 panel for the Linux desktop.

fbpanel draws a configurable panel bar — taskbar, system tray, launcher,
clock, pager, resource monitors, and more — built from small, independent
plugins loaded at startup.  It is a plain X11 application with no desktop
environment dependency.

## Screenshot

![screenshot](/data/shot.png)

## History

fbpanel was originally written by Anatoly Asviyan
([aanatoly](https://github.com/aanatoly/fbpanel)).  When
development stalled and the project was abandoned,
[eleksir](https://github.com/eleksir/fbpanel) picked it up (~2020) and made
substantial improvements: migrated the build from the old Python 2 autotools
system to CMake, added the `batterytext` and `user` plugins, rewrote the
battery backend to use `/sys` instead of `/proc`, added pager icon drawing,
fixed a pile of compiler warnings and GLib deprecations, and shipped the
result as v7.2.

This repository imported eleksir's v7.2 as its starting point (v8.0.0),
then continued from there with further bug fixes, refactoring, and active
maintenance.

## Plugins

| Plugin | Description |
|--------|-------------|
| `battery` | Battery charge level — icon style |
| `batterytext` | Battery charge level — text/numeric style |
| `chart` | Scrolling bar-chart base (used by cpu, net, mem2) |
| `cpu` | CPU usage chart |
| `dclock` | Digital clock using pixel-art bitmap glyphs |
| `deskno` | Current virtual desktop number |
| `deskno2` | Current virtual desktop name |
| `genmon` | Generic monitor — runs a command and displays its output |
| `icons` | Invisible plugin: override per-application window icons |
| `image` | Static image |
| `launchbar` | Application launcher bar |
| `mem` | Memory usage (progress-bar style) |
| `mem2` | Memory usage (chart style) |
| `menu` | Application menu button ("start menu") |
| `meter` | Internal base plugin for icon-level meters |
| `net` | Network traffic monitor |
| `pager` | Virtual desktop pager (thumbnail miniatures) |
| `separator` | Visual separator |
| `space` | Blank spacer |
| `taskbar` | One button per open window; raise/iconify/close |
| `tclock` | Text clock using GTK/Pango (honours the theme font) |
| `tray` | Freedesktop system notification area (system tray) |
| `user` | User avatar with popup action menu |
| `volume` | OSS/ALSA volume control |
| `wincmd` | "Show Desktop" button |

## Installation

### From a pre-built package

Each tagged release publishes pre-built packages on the
[Releases page](https://github.com/Fullaxx/fbpanel/releases).

#### Debian / Ubuntu (`.deb`)

| Distro | Asset filename |
|--------|----------------|
| Ubuntu 24.04 (Noble) | `fbpanel_<version>_amd64_noble.deb` |
| Ubuntu 22.04 (Jammy) | `fbpanel_<version>_amd64_jammy.deb` |
| Ubuntu 20.04 (Focal) | `fbpanel_<version>_amd64_focal.deb` |
| Debian 13 (Trixie) | `fbpanel_<version>_amd64_trixie.deb` |
| Debian 12 (Bookworm) | `fbpanel_<version>_amd64_bookworm.deb` |
| Debian 11 (Bullseye) | `fbpanel_<version>_amd64_bullseye.deb` |

```bash
sudo dpkg -i fbpanel_<version>_amd64_<distro>.deb
sudo apt-get install -f   # resolve any missing dependencies
```

#### Fedora (`.rpm`)

| Distro | Asset filename |
|--------|----------------|
| Fedora 43 | `fbpanel-<version>-1.x86_64_fedora43.rpm` |
| Fedora 42 | `fbpanel-<version>-1.x86_64_fedora42.rpm` |
| Fedora 41 | `fbpanel-<version>-1.x86_64_fedora41.rpm` |
| Fedora 40 | `fbpanel-<version>-1.x86_64_fedora40.rpm` |

```bash
sudo dnf install fbpanel-<version>-1.x86_64_<distro>.rpm
```

### From source

See [INSTALL.md](INSTALL.md) for full build instructions and dependency list.
Quick start:

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j$(nproc)
sudo make -C build install
```

## Contributing

Bug reports and pull requests are welcome.

## Related projects

- [aanatoly/fbpanel](https://github.com/aanatoly/fbpanel) — original repository by the project's author
- [eleksir/fbpanel](https://github.com/eleksir/fbpanel) — GTK2 fork that carried the project through v7.2
- [fbpanel/fbpanel](https://github.com/fbpanel/fbpanel) — another independent GTK2 maintenance fork
- [berte/fbpanel3](https://github.com/berte/fbpanel3) — GTK3 port of fbpanel
