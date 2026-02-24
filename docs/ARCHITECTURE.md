# fbpanel — Architecture Reference

fbpanel is a lightweight desktop panel for Linux/X11, built on GTK 2.0.
It uses a plugin-based architecture: the core binary provides a framework
and an X11-managed window, while all panel contents (clocks, taskbars,
launchers, etc.) are implemented as dynamically loaded shared libraries.

---

## Repository layout

```
fbpanel/
├── panel/          Core panel binary (compiled into the fbpanel executable)
├── plugins/        Plugin shared libraries (one subdirectory per plugin)
├── data/           Config templates, man page sources, default XPM icon
├── po/             Gettext translation files (.po / .mo)
├── exec/           Helper shell scripts installed alongside the binary
├── contrib/        Distribution packaging files (Gentoo ebuilds)
├── docs/           Documentation (this directory)
└── www/            Website source
```

---

## High-level design

```
┌─────────────────────────────────────────────────────────┐
│                       fbpanel process                    │
│                                                          │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────────┐ │
│  │  panel.c │   │  misc.c  │   │     xconf.c          │ │
│  │ (main,   │   │ (X11/GTK │   │  (config parser)     │ │
│  │  window) │   │  utils)  │   │                      │ │
│  └────┬─────┘   └──────────┘   └──────────────────────┘ │
│       │                                                   │
│       │  dlopen each plugin .so                          │
│       ▼                                                   │
│  ┌──────────────────────────────────────────────────┐    │
│  │             GtkWindow (panel bar)                 │    │
│  │  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────┐  │    │
│  │  │ clock  │ │taskbar │ │ pager  │ │   tray   │  │    │
│  │  │ plugin │ │ plugin │ │ plugin │ │  plugin  │  │    │
│  │  └────────┘ └────────┘ └────────┘ └──────────┘  │    │
│  └──────────────────────────────────────────────────┘    │
│                                                           │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────────┐ │
│  │  ev.c    │   │  bg.c    │   │   gtkbgbox.c         │ │
│  │ (EWMH    │   │ (root bg │   │ (pseudo-transparent  │ │
│  │  events) │   │  pixmap) │   │  widget base)        │ │
│  └──────────┘   └──────────┘   └──────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

---

## Core panel — `panel/`

### `panel.c` / `panel.h`

The main entry point and orchestrator.

**Responsibilities:**
- Parses command-line arguments and locates the profile config file
  (default: `~/.config/fbpanel/default`).
- Creates the top-level `GtkWindow` and sets the X11 window type/strut
  properties so window managers treat it as a dock (reserves screen space).
- Calls `fb_init()`: interns X11 atoms, initialises the icon theme and
  the EWMH event bus (`fbev`).
- Reads the profile config (`xconf` tree), then instantiates every plugin
  listed in `Plugin { }` config blocks.
- Configures autohide if enabled, sets panel geometry and transparency.
- Enters the GTK main loop.
- On exit, calls `fb_free()` and destroys all plugin instances.

**Key types defined in `panel.h`:**

| Type | Purpose |
|------|---------|
| `panel` | Global struct: geometry, GTK widgets, plugin list, orientation, monitor |
| `net_wm_state` | Bitfield parsed from `_NET_WM_STATE` |
| `net_wm_window_type` | Bitfield parsed from `_NET_WM_WINDOW_TYPE` |
| `xconf_enum` | String↔int mapping used by the config parser |

**Key globals exported from `panel.h`:**

| Symbol | Type | Meaning |
|--------|------|---------|
| `icon_theme` | `GtkIconTheme *` | Default GTK icon theme; all icon loading uses this |
| `fbev` | `FbEv *` | EWMH event bus GObject |
| `the_panel` | `panel *` | Pointer to the single panel instance |

All X11 atoms are declared `extern Atom a_NET_*` in `ewmh.h` and
defined in `ewmh.c:resolve_atoms()`.

---

### `ewmh.c` / `ewmh.h`

X11 atom table and EWMH/ICCCM property helpers.  Split from `misc.c` in v8.1.7.

**Responsibilities:**
- Declares and interns all `extern Atom a_NET_*` / `a_WM_*` atoms via
  `resolve_atoms()` (called from `fb_init()`).
- Provides `GDK_DPY` macro: `GDK_DISPLAY_XDISPLAY(gdk_display_get_default())`.
- X11 message helpers: `Xclimsg()`, `Xclimsgwm()`.
- Property readers: `get_xaproperty()`, `get_utf8_property()`,
  `get_utf8_property_list()`, `get_textproperty()`.
- EWMH accessors: `get_net_current_desktop()`, `get_net_wm_desktop()`,
  `get_net_wm_state()`, `get_net_wm_window_type()`, `get_net_number_of_desktops()`.

---

### `fbwidgets.c` / `fbwidgets.h`

GTK widget factory helpers.  Split from `misc.c` in v8.1.7.

**Responsibilities:**
- Pixbuf/button factory: `fb_pixbuf_new()`, `fb_image_new()`, `fb_button_new()`.
- Calendar helper: `fb_create_calendar()`.
- Color utilities: `gcolor2rgb24()`, `gdk_color_to_RRGGBB()`.
- GTK metric: `get_button_spacing()`.

---

### `misc.c` / `misc.h`

Genuine panel utilities.  `misc.h` includes `ewmh.h` and `fbwidgets.h` for
backward compatibility so plugin code need not change its `#include` directives.

**Responsibilities:**
- Panel geometry: `calculate_position()` — places the panel window on screen
  according to edge/align/margin/size config.
- String utilities: `expand_tilda()`, `indent()`.
- Enum / string lookup tables: `str2num()`, `num2str()` and the various
  `bool_enum`, `edge_enum`, `align_enum` tables used by `XCG()`.

---

### `plugin.c` / `plugin.h`

Plugin registry and lifecycle management.

**Responsibilities:**
- Maintains a global list of `plugin_class` descriptors (statically
  registered at `dlopen` time by the `PLUGIN` macro).
- `class_register()` / `class_unregister()` — called by the `ctor()`/`dtor()`
  shared-library constructor/destructor generated by the `PLUGIN` macro.
- `plugin_load()` — allocates `priv_size` bytes, sets `plugin_instance` fields
  (`panel`, `xc`, `pwid`), then calls the plugin's `constructor`.
- `plugin_put()` / `plugin_stop()` — call the plugin's `destructor` and free.

**Key types:**

| Type | Purpose |
|------|---------|
| `plugin_class` | Static descriptor: type string, name, priv_size, ctor/dtor pointers |
| `plugin_instance` | Per-instance state: class ptr, panel ptr, xconf subtree, pwid widget |

---

### `ev.c` / `ev.h`

EWMH event bus.

**Responsibilities:**
- Defines `FbEv`, a custom `GObject` subclass acting as a signal bus.
- Receives X11 `PropertyNotify` events on the root window via a GDK event
  filter and translates them to GObject signals:

| Signal | Fired when |
|--------|-----------|
| `current_desktop` | `_NET_CURRENT_DESKTOP` changes |
| `active_window` | `_NET_ACTIVE_WINDOW` changes |
| `number_of_desktops` | `_NET_NUMBER_OF_DESKTOPS` changes |
| `client_list` | `_NET_CLIENT_LIST` changes |
| `desktop_names` | `_NET_DESKTOP_NAMES` changes |

Plugins connect to these signals instead of installing their own
root-window X11 event filters.

---

### `bg.c` / `bg.h`

Root window background pixmap reader.

**Responsibilities:**
- Reads the current desktop wallpaper from the `_XROOTPMAP_ID` X11 property.
- Monitors the root window for wallpaper changes and notifies listeners.
- Provides a GdkPixmap of the root window for pseudo-transparent rendering.

---

### `gtkbgbox.c` / `gtkbgbox.h`

Custom GTK widget: pseudo-transparent panel background.

**Responsibilities:**
- `GtkBgbox` is a `GtkEventBox` subclass.
- Overrides the GTK2 `expose_event` vfunc to paint its background by
  copying the appropriate region of the root window pixmap (via `FbBg`),
  optionally tinted with a colour and alpha value.
- `gtk_bgbox_set_background()` configures the background mode:
  `BG_NONE`, `BG_STYLE`, `BG_ROOT` (root pixmap sample), or
  `BG_INHERIT` (copy from parent).
- All plugin `pwid` widgets are `GtkBgbox` instances, giving the whole
  panel a consistent transparent-or-tinted appearance.

---

### `gtkbar.c` / `gtkbar.h`

Custom flow-layout container.

**Responsibilities:**
- `GtkBar` is a `GtkBox` subclass.
- Arranges children in rows/columns, wrapping to the next row/column
  when the available space is exceeded.
- `gtk_bar_set_dimension()` sets the number of columns (horizontal panel)
  or rows (vertical panel).
- Used by taskbar and launchbar to tile buttons.

---

### `xconf.c` / `xconf.h`

Config file parser.

**Responsibilities:**
- Parses fbpanel's whitespace-delimited, brace-nested config format.
- Builds a tree of `xconf` nodes that can be traversed by plugins.
- `xconf_find()` / `XCG()` macro — type-safe helpers to read typed values
  (int, string, enum) from a plugin's config subtree.
- `xconf_write()` — serialises the config tree back to disk (used by the
  preferences dialog).

---

### `gconf.c` / `gconf.h` / `gconf_panel.c` / `gconf_plugins.c`

GTK preferences dialog ("Configure Panel").

| File | Responsibility |
|------|---------------|
| `gconf.c` | Dialog skeleton, helper widget constructors |
| `gconf_panel.c` | "Panel" tab: edge, alignment, size, transparency, autohide |
| `gconf_plugins.c` | "Plugins" tab: list of active plugins, add/remove/reorder |

---

### `run.c` / `run.h`

Shell command helper.

**Responsibilities:**
- `run_app()` — spawns a shell command asynchronously via
  `g_spawn_command_line_async()`.
- `run_app_argv()` — same but takes an `argv` array.
- Used by launchbar, menu, and wincmd plugins.

---

### `dbg.h`

Debug trace macros.

| Macro | Behaviour |
|-------|----------|
| `DBG(fmt, ...)` | Prints with file/line prefix when `DEBUGPRN` is `#define`d |
| `ERR(fmt, ...)` | Always prints to stderr |
| `DBGE(fmt, ...)` | Like `DBG` but no trailing newline |

---

## Plugin system — `plugins/`

Each plugin is a standalone shared library loaded by `GModule` (dlopen).
The plugin exports a single `plugin_class *class_ptr` symbol and calls
`class_register()` on load via the `PLUGIN` macro.

### Plugin inventory

| Plugin | Description |
|--------|-------------|
| `battery` | Battery level via icon meter (reads `/sys/class/power_supply`) |
| `batterytext` | Battery level as text label |
| `brightness` | Backlight brightness label + scroll-to-adjust (`/sys/class/backlight`) |
| `chart` | Reusable scrolling bar-graph widget (used by cpu/mem/net/diskio) |
| `cpu` | CPU usage bar graph (reads `/proc/stat`) |
| `cpufreq` | CPU clock frequency label (`/sys/devices/system/cpu/cpuN/cpufreq`) |
| `dclock` | Digital clock label with optional calendar popup |
| `deskno` | Current virtual desktop number label |
| `deskno2` | Alternative desktop number format |
| `diskio` | Disk read/write throughput chart (reads `/proc/diskstats`) |
| `diskspace` | Filesystem usage progress bar (`statvfs(3)`) |
| `genmon` | Generic external command output display |
| `icons` | Row of open window icons |
| `image` | Static icon from file or theme |
| `launchbar` | Row of icon buttons that launch commands |
| `loadavg` | System load average label (reads `/proc/loadavg`) |
| `mem` | Memory usage bar graph (reads `/proc/meminfo`) |
| `mem2` | Memory usage text label |
| `menu` | Application menu from freedesktop .menu files |
| `meter` | Reusable icon-based level meter (used by battery/volume) |
| `net` | Network traffic dual bar graph (reads `/proc/net/dev`) |
| `pager` | Virtual desktop pager (miniature desktop view) |
| `separator` | Visual separator line |
| `space` | Expanding spacer |
| `swap` | Swap usage progress bar (reads `/proc/meminfo`; hides if no swap) |
| `taskbar` | Window taskbar (EWMH client list) |
| `tclock` | Analog clock drawn on a GtkDrawingArea |
| `thermal` | CPU/board temperature label (`/sys/class/thermal`; colour-coded) |
| `tray` | System tray (freedesktop XEMBED protocol) |
| `user` | Current username label |
| `volume` | ALSA master volume control |
| `wincmd` | Send EWMH commands to windows |
| `windowtitle` | Active window title label (EWMH `_NET_ACTIVE_WINDOW`) |

---

## Startup sequence

```
main()
  └── fb_init()                    // intern atoms, setup FbEv, icon theme
  └── panel_read_config()          // parse ~/.config/fbpanel/<profile>
  └── panel_start_gui()            // create GtkWindow, set struts
  └── for each Plugin block:
        plugin_load()              // dlopen .so, call constructor
        gtk_container_add()        // add plugin pwid to panel hbox
  └── gtk_main()                   // enter event loop

on gtk_main_quit():
  └── for each plugin:
        plugin_stop()              // call destructor, gtk_widget_destroy(pwid)
  └── fb_free()                    // cleanup FbEv, FbBg, atoms
```

---

## X11 integration

The panel window is created as a regular `GtkWindow` then its X11 window
type and struts are set directly:

```c
// Set window type to _NET_WM_WINDOW_TYPE_DOCK
// so window managers don't tile/decorate it
XChangeProperty(dpy, xwin, a_NET_WM_WINDOW_TYPE, XA_ATOM, 32,
    PropModeReplace, (guchar *) &a_NET_WM_WINDOW_TYPE_DOCK, 1);

// Set _NET_WM_STRUT / _NET_WM_STRUT_PARTIAL so WMs
// reserve the panel's screen space (avoid overlap)
panel_set_wm_strut(p);
```

The `FBPANEL_WIN(xid)` macro (defined in `panel.h`) tests whether a
given X window ID belongs to the panel itself — used by the taskbar to
exclude the panel from the window list.

---

## Multi-monitor support

The panel queries `gdk_screen_get_monitor_geometry()` to determine the
working area of the monitor it is configured to occupy (`monitor` field
in the panel config). Geometry calculations in `calculate_position()`
use these boundaries.

---

## Internationalization

The panel uses gettext. Translatable strings are marked with `c_()` macro
(defined in `panel.h` as `gettext()`). Translation `.mo` files for French,
Russian, and Italian are installed by CMake.

---

## Build system

CMake (>= 3.5) with the following targets:

| Target | Output |
|--------|--------|
| `fbpanel` | Main executable: `/usr/bin/fbpanel` |
| `lib<plugin>.so` | Plugin shared library: `/usr/lib/fbpanel/` |
| Data files | `/usr/share/fbpanel/` (images, config templates) |
| Man page | `/usr/share/man/man1/fbpanel.1` |
| Locale | `/usr/share/locale/*/LC_MESSAGES/fbpanel.mo` |

CPack generates a Debian `.deb` package for distribution.

**Build:**
```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
make -C build -j$(nproc)
sudo make -C build install
# or: cd build && cpack -G DEB
```
