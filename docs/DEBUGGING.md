# fbpanel — Debugging Guide

---

## Debug build

Build with debug symbols and assertions enabled:

```bash
cmake -B build \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-O0 -g3 -Wall -Wextra"
make -C build -j$(nproc)
```

For a RelWithDebInfo build (optimised but with symbols — useful for
profiling and stack traces in production):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

---

## Running fbpanel from the build directory

After building (without installing):

```bash
# Run fbpanel pointing at the local build directory
./build/fbpanel --profile default \
    --plugin-path ./build   # if panel searches for plugins here
```

Or set `LD_LIBRARY_PATH` so the shell finds the plugin `.so` files:

```bash
LD_LIBRARY_PATH=./build ./build/fbpanel
```

---

## DBG() tracing

fbpanel uses a compile-time trace macro defined in `dbg.h`:

```c
// Enable tracing for a specific file by adding at the top:
#define DEBUGPRN
#include "dbg.h"

// Then use in code:
DBG("value = %d\n", value);
ERR("fatal: %s\n", strerror(errno));
```

`DBG()` outputs: `filename:line_number: message`

To enable debugging in a specific plugin without rebuilding everything,
add `#define DEBUGPRN` at the top of that plugin's `.c` file and
rebuild only that plugin.

---

## Verbose GTK/GLib output

```bash
# Log all GLib/GObject messages
G_MESSAGES_DEBUG=all ./build/fbpanel 2>&1 | tee fbpanel.log

# GTK debug flags
GDK_DEBUG=all ./build/fbpanel
GTK_DEBUG=all ./build/fbpanel
```

Useful `GTK_DEBUG` values:
- `misc` — miscellaneous info
- `keybindings` — key event tracing
- `updates` — widget redraw regions
- `geometry` — widget size allocation

---

## Address Sanitizer (ASAN)

Detects use-after-free, buffer overflows, and memory leaks:

```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -g3 -fno-omit-frame-pointer"
make -C build -j$(nproc)

ASAN_OPTIONS=detect_leaks=1 ./build/fbpanel 2>&1 | tee asan.log
```

For leak detection on exit (requires LeakSanitizer):

```bash
LSAN_OPTIONS=suppressions=lsan.supp ./build/fbpanel
```

---

## Valgrind

```bash
valgrind --leak-check=full \
         --track-origins=yes \
         --show-leak-kinds=all \
         --suppressions=/usr/lib/valgrind/gtk.supp \
         ./build/fbpanel 2>&1 | tee valgrind.log
```

GTK2 generates many "reachable" leak reports from its static
initialisation. Use GTK suppressions to filter them.

---

## GDB

```bash
gdb ./build/fbpanel

(gdb) run --profile default
# ... wait for crash or set a breakpoint
(gdb) bt full        # full stack trace
(gdb) info locals    # local variables
(gdb) p *the_panel   # inspect global panel struct
```

Useful GDB breakpoints:

```gdb
b panel.c:fb_init
b plugin.c:plugin_load
b panel.c:panel_start_gui
b dclock.c:dclock_constructor
```

---

## X11 debugging

### Watch X properties in real-time

```bash
# Monitor all X11 property changes on the root window:
xprop -root -spy

# Read a specific EWMH property:
xprop -root _NET_CLIENT_LIST
xprop -root _NET_CURRENT_DESKTOP
xprop -id $(xdotool getactivewindow) _NET_WM_STATE
```

### xwininfo — inspect the panel X window

```bash
# Get panel window ID:
xwininfo -name fbpanel

# Detailed info:
xwininfo -all -id <window_id>
```

### xdpyinfo — display info

```bash
xdpyinfo | grep -A4 "screen #0"
```

---

## Plugin loading issues

If a plugin fails to load, fbpanel logs a message to stderr:

```
g_module_open failed: /usr/lib/fbpanel/libbattery.so: cannot open shared object file
```

Check:
```bash
# Verify the plugin exists:
ls -la /usr/lib/fbpanel/

# Check for missing dependencies:
ldd /usr/lib/fbpanel/libbattery.so

# Look for unresolved symbols:
nm -D /usr/lib/fbpanel/libbattery.so | grep ' U '
```

---

## Config file debugging

Run fbpanel with an explicit config file:

```bash
./build/fbpanel --config /tmp/test.conf
```

To validate config parsing without launching the full panel:
- Check stderr for "unknown key" or "parse error" messages.
- The parser is lenient — unknown keys are silently ignored.

---

## Pseudo-transparency issues

If the panel background doesn't update when the wallpaper changes:

1. Check that `_XROOTPMAP_ID` is being set by your wallpaper setter:
   ```bash
   xprop -root _XROOTPMAP_ID
   ```

2. Ensure `transparent = true` and `alpha` is set appropriately in config.

3. Send `SIGUSR1` to force a refresh:
   ```bash
   killall -USR1 fbpanel
   ```

4. Some wallpaper setters use `_XSETROOT_ID` instead — fbpanel checks
   `_XROOTPMAP_ID` only.

---

## Autohide timing

If autohide is jittery or doesn't work:

1. Increase `height_when_hidden` (minimum 1px required for mouse events).
2. Check that no other window is overlapping the hidden panel edge.
3. The hide delay is hardcoded — search `panel.c` for `HIDE_TIMEOUT`.

---

## Common crash scenarios

| Crash | Likely cause |
|-------|-------------|
| SIGSEGV in plugin callback after panel exit | Timer not removed in destructor |
| SIGSEGV in signal handler | Signal not disconnected from `fbev` in destructor |
| X11 BadWindow error | Plugin using a window XID after the window was destroyed |
| Double-free in GtkWidget | Plugin calling `gtk_widget_destroy(p->pwid)` |
| Crash in tray icon handling | Race condition: tray app exits during embedding |

---

## Reporting a bug

Collect the following before reporting:

1. fbpanel version: `fbpanel --version`
2. GTK2 version: `pkg-config --modversion gtk+-2.0`
3. Config file: `~/.config/fbpanel/default`
4. Stack trace (GDB or ASAN output)
5. X11 WM name: `xprop -root _NET_WM_NAME`
6. Distribution and kernel version: `uname -a`

File issues at the project GitHub repository.
