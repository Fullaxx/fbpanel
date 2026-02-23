# New plugin ideas

Plugins are grouped by implementation complexity and dependency footprint.
"No new deps" means the plugin needs only what fbpanel already links against:
GTK2, GLib, X11, GModule, and the C standard library.

---

## Tier 1 — Low complexity, no new dependencies

These read from `/proc` or `/sys`, or use existing X11/EWMH infrastructure
already wired up in fbpanel.  Any of these could be written in an afternoon.

### `windowtitle`
Show the title of the currently active window in a label on the panel.
- Data source: `_NET_ACTIVE_WINDOW` + `_NET_WM_NAME` — both already tracked
  by fbpanel's event bus (`FbEv`).  Connect to `EV_ACTIVE_WINDOW`.
- No polling needed; purely event-driven.
- Optional: truncate to N characters, configurable font/colour via Pango markup.

### `thermal`
Show CPU/board temperature as an icon level (hot/warm/cool) or text.
- Data source: `/sys/class/thermal/thermal_zone*/temp` or
  `/sys/class/hwmon/hwmon*/temp*_input` (poll every 5 s).
- Can reuse the `meter` base class for icon-level display, exactly like
  `battery` does.
- Configurable: which thermal zone to watch, warn/critical thresholds.

### `cpufreq`
Show current CPU clock frequency as text (e.g. "3.4 GHz").
- Data source: `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`
  (poll every 2 s).
- Can show aggregate (max across cores) or a specific core.
- Trivially simple; no base class needed.

### `diskspace`
Show used/free space for a configured mount point as a progress bar or text.
- Data source: `statvfs(3)` — no file reads, one syscall.
- Similar structure to `mem` (progress bar, configurable path).
- Configurable: mount point, warn threshold, display format.

### `diskio`
Scrolling chart of disk read/write throughput, matching the style of `net`.
- Data source: `/proc/diskstats` (poll every 1 s, diff successive samples).
- Can reuse the `chart` base plugin directly — `net` and `cpu` already do this.
- Configurable: which block device(s) to watch, separate read/write colours.

### `loadavg`
Show 1-, 5-, and 15-minute load averages from `/proc/loadavg` as text.
- Simplest possible plugin: one `g_timeout_add`, one `gtk_label_set_text`.
- Configurable: which averages to display, colour threshold.

### `swap`
Show swap usage as a progress bar.
- Data source: `/proc/meminfo` — already parsed by the `mem` plugin.
- Could literally share `mem`'s parsing logic, or be a separate standalone.

### `brightness`
Adjust screen backlight brightness via scroll wheel, show level as an icon.
- Data source: `/sys/class/backlight/<device>/brightness` and
  `max_brightness` (read/write).
- Modelled after `volume`: scroll-wheel changes level, click to toggle
  full/minimum.
- No new library needed if using sysfs directly (requires write permission
  to the sysfs node, or a setuid helper).

---

## Tier 2 — Medium complexity, uses existing GTK2 features or one small dep

These require non-trivial GTK2 work, a new popup window, or a small external
library that is almost universally available.

### `calendar`
Popup calendar on clicking the clock.
- GTK2 already ships `GtkCalendar` — no new dependency.
- Create a `GtkWindow` (or `GtkDialog`) containing a `GtkCalendar` on button
  press; destroy it on focus-out.
- Could be implemented as a wrapper around `tclock` or `dclock` rather than a
  fully independent plugin.

### `kbdlayout`
Show and cycle keyboard layouts (e.g. "us → de → fr").
- Data source: XKB extension (`XkbGetState`, `XkbGetNames`) — X11 already
  linked.
- Click cycles to the next layout; right-click shows a menu of all configured
  layouts.
- Medium X11 complexity: XKB API is verbose but well-documented.

### `timer`
A configurable countdown timer with a visual indicator.
- Pure GLib + GTK2: `g_timeout_add`, a `GtkProgressBar` or label, a small
  config popup on right-click.
- States: idle → running → alarmed (flashing/colour change).
- No new deps; self-contained GLib timer logic.

### `clipboard`
Show the last N clipboard entries in a popup menu; click to re-paste.
- Data source: X11 clipboard (`GDK_SELECTION_CLIPBOARD`,
  `gdk_selection_convert`) — X11 already linked.
- Medium complexity: clipboard monitoring in X11 requires handling
  `SelectionNotify` events and dealing with INCR transfers for large content.
- Text-only to start; image support would bump this to Tier 3.

### `weather`
Show current conditions (temperature + icon) fetched from a web service.
- Simplest approach: shell out to `curl wttr.in/?format="%t+%C"` via
  `g_spawn_async` and display the result — essentially a specialised `genmon`
  with a fixed command and an icon.
- Built-in HTTP approach: add `libcurl` as an optional dependency; fetch in a
  `GThreadPool` worker to avoid blocking the GTK main loop.
- Configurable: location, units (°C/°F), update interval, service URL.

### `gpu`
Show GPU utilisation and/or VRAM usage.
- AMD/Intel: `/sys/class/drm/card*/device/gpu_busy_percent` or
  `/sys/kernel/debug/dri/*/` — no new deps.
- NVIDIA: `nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader`
  via `g_spawn` — no new deps but nvidia-smi must be present.
- Complexity is mostly in handling the three different vendor paths gracefully.

---

## Tier 3 — Higher complexity, significant new dependencies

These require D-Bus communication or other non-trivial IPC.  Each would add
a new build dependency (`libdbus-1`, `libgio-2.0`, etc.).  GLib 2.26+
ships `GDBus` (part of GIO/GLib), so if a minimum GLib version is acceptable
these can be done without a separate dbus library.

### `mpris`
Media player controller: show song title/artist, play/pause, skip buttons.
- Protocol: MPRIS2 over D-Bus (`org.mpris.MediaPlayer2.Player`).
- Works with any MPRIS2-compliant player (mpd, vlc, spotify, rhythmbox, etc.).
- Dependency: GDBus (part of GLib/GIO ≥ 2.26) — likely already available on
  target distros.
- Complexity: D-Bus property watching (`PropertiesChanged` signal),
  method calls for transport control, metadata parsing.

### `notifications`
Show unread notification count; click to see/dismiss recent notifications.
- Protocol: implement a minimal D-Bus notification server
  (`org.freedesktop.Notifications`) or query an existing one.
- Acting as a full notification daemon is high complexity.  A simpler first
  step is to proxy/count notifications from an existing daemon that exposes a
  count (e.g. dunst's `com.dunst.Notification.notify` signals).
- Dependency: GDBus.

### `upower`
Detailed power management: multiple batteries, AC adapter status, suspend/
hibernate buttons, time-to-empty/time-to-full.
- Protocol: UPower D-Bus API (`org.freedesktop.UPower`).
- Dependency: GDBus + UPower daemon (standard on most desktops).
- This would supersede the existing `battery` plugin for systems where UPower
  is available, while `battery` remains the sysfs fallback.

### `networkmanager`
Show Wi-Fi signal strength and SSID; click for a connection picker.
- Protocol: NetworkManager D-Bus API (`org.freedesktop.NetworkManager`).
- Dependency: GDBus + NetworkManager daemon.
- High complexity: signal strength polling, AP list enumeration, WPA
  passphrase entry (needs a `GtkDialog` or defers to `nm-applet`).
- A read-only indicator (SSID + signal icon, no connection switching) is
  medium complexity and covers most use cases.

### `pulseaudio`
Volume control via PulseAudio (replaces the current OSS/ALSA `volume` plugin
for PulseAudio systems).
- Dependency: `libpulse` (`libpulse-dev` / `pulseaudio-libs-devel`).
- Features the current `volume` plugin cannot do: per-application volume,
  sink selection, mute indicator for the default sink.
- The existing `volume` plugin continues to work for ALSA/OSS systems.
