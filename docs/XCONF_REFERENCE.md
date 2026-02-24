# fbpanel — Configuration File Reference

fbpanel uses its own simple text-based configuration format called `xconf`.
This document describes the syntax, the global panel options, and all
per-plugin configuration keys.

---

## Config file location

```
~/.config/fbpanel/<profile>
```

The default profile is named `default`. A different profile can be
selected with:
```bash
fbpanel --profile myprofile
```

If no config file exists, fbpanel will try to copy a default config
from `$DATADIR/fbpanel/config/default` (typically
`/usr/share/fbpanel/config/default`).

---

## Syntax

The format is a whitespace-delimited, brace-nested key-value language:

```
BlockName {
    key = value
    key = value
    NestedBlock {
        key = value
    }
}
```

Rules:
- Block names and keys are case-sensitive identifiers.
- Values are unquoted strings (terminated by newline or `}`).
- Comments use `#` (rest of line is ignored).
- Strings containing spaces must NOT be quoted — the parser does not
  handle quotes. Use filenames without spaces.
- Integer values are parsed by `atoi()`.
- Boolean values: `1` / `true` / `yes` = true; `0` / `false` / `no` = false.

---

## Global panel block

The top-level block is `Global`:

```
Global {
    edge        = bottom     # Panel edge: top | bottom | left | right
    align       = center     # Alignment: left | right | center
    margin      = 0          # Margin in pixels from edge alignment point
    widthtype   = percent    # Width unit: percent | pixel
    width       = 100        # Panel width in widthtype units
    height      = 28         # Panel height in pixels
    transparent = false      # Pseudo-transparent background
    tintcolor   = #FFFFFF    # Tint color (hex RGB) when transparent
    alpha       = 0          # Tint alpha 0-255 (0=opaque tint, 255=fully transparent)
    setdocktype = true       # Set _NET_WM_WINDOW_TYPE_DOCK
    setpartialstrut = true   # Set _NET_WM_STRUT_PARTIAL
    autohide    = false      # Auto-hide panel when mouse leaves
    height_when_hidden = 2   # Height in pixels when auto-hidden
    roundcorners = false     # (reserved, not implemented)
    monitor     = 0          # Monitor index (0 = primary)
    layer       = normal     # Stacking layer: normal | above | below
    iconsize    = 24         # Default icon size in pixels
    background  = false      # Use background image
    backgroundfile =         # Path to background image
    font        =            # Font description (Pango format, e.g. "Sans 10")
    fontcolor   = #000000    # Font color for plugins that use it
}
```

### Edge values

| Value | Meaning |
|-------|---------|
| `top` | Panel docks to the top of the screen |
| `bottom` | Panel docks to the bottom (default) |
| `left` | Panel docks to the left edge (vertical) |
| `right` | Panel docks to the right edge (vertical) |

### Alignment values

| Value | Meaning |
|-------|---------|
| `left` | Align to left (or top for vertical) |
| `center` | Center on edge |
| `right` | Align to right (or bottom for vertical) |

---

## Plugin blocks

Each `Plugin { }` block instantiates one plugin:

```
Plugin {
    type    = dclock         # Plugin type string (required)
    expand  = false          # TRUE: expand to fill available space
    padding = 0              # Extra padding around plugin in pixels
    Config {
        # Plugin-specific keys go here
    }
}
```

Plugins are instantiated in the order they appear in the config file,
from left to right (or top to bottom for vertical panels).

---

## Plugin configuration reference

### `dclock` — Digital Clock

```
Plugin {
    type = dclock
    Config {
        ClockFmt   = %R         # strftime format string (default: %R = HH:MM)
        TooltipFmt = %A %x %R  # Tooltip strftime format
        Action     =            # Command to run on click (default: calendar)
        BoldFont   = true       # Use bold font
        IconOnly   = false      # Show only an icon, not text
        ShowCalendar = true     # Open calendar popup on click
    }
}
```

### `tclock` — Analog Clock

```
Plugin {
    type = tclock
    Config {
        Size       = 32         # Clock diameter in pixels
        ShowSecond = true       # Draw second hand
    }
}
```

### `taskbar` — Window Taskbar

```
Plugin {
    type = taskbar
    expand = true
    Config {
        MaxTaskWidth  = 150     # Maximum button width in pixels
        TasksAll      = false   # Show windows from all desktops
        IconsOnly     = false   # Show only icons (no text labels)
        ShowIconified = true    # Include minimized windows
        UseMouseWheel = true    # Scroll wheel switches windows
        GroupedTasks  = false   # (reserved)
    }
}
```

### `pager` — Virtual Desktop Pager

```
Plugin {
    type = pager
    Config {
        ShowDesktop  = true     # Show desktop backgrounds in miniatures
        AspectRatio  = true     # Maintain screen aspect ratio in miniatures
    }
}
```

### `tray` — System Tray

```
Plugin {
    type = tray
    # No plugin-specific config keys
}
```

### `menu` — Application Menu

```
Plugin {
    type = menu
    Config {
        image  = start-here     # Icon name or path for menu button
        system = true           # Include system .menu file entries
        Config {
            # Menu items:
            item {
                image  = terminal
                name   = Terminal
                action = xterm
            }
            separator { }
            item {
                image  = logout
                name   = Logout
                action = xlogout
            }
        }
    }
}
```

### `launchbar` — Application Launcher

```
Plugin {
    type = launchbar
    Config {
        Button {
            image   = firefox       # Icon name or file path
            tooltip = Web Browser   # Tooltip text
            action  = firefox       # Command to run on click
        }
        Button {
            image   = xterm
            tooltip = Terminal
            action  = xterm
        }
    }
}
```

### `battery` — Battery Meter (icon)

```
Plugin {
    type = battery
    Config {
        HideIfNoBattery = true   # Hide plugin if no battery found
        AlarmLevel      = 5      # Percentage threshold for alarm
        AlarmCommand    =        # Command to run at alarm level
    }
}
```

### `batterytext` — Battery Meter (text)

```
Plugin {
    type = batterytext
    Config {
        HideIfNoBattery = true
        Tooltip = true          # Show percentage in tooltip
    }
}
```

### `cpu` — CPU Usage

```
Plugin {
    type = cpu
    Config {
        Color   = #00FF00       # Bar color (hex RGB)
        Width   = 40            # Bar graph width in pixels
    }
}
```

### `mem` — Memory Usage (bar)

```
Plugin {
    type = mem
    Config {
        Color   = #0000FF
        Width   = 40
    }
}
```

### `mem2` — Memory Usage (text)

```
Plugin {
    type = mem2
    Config {
        Tooltip = true          # Show details in tooltip
    }
}
```

### `net` — Network Traffic

```
Plugin {
    type = net
    Config {
        Interface = eth0        # Network interface to monitor
        Color1    = #00FF00     # Upload bar color
        Color2    = #0000FF     # Download bar color
        Width     = 40
    }
}
```

### `genmon` — Generic Monitor

```
Plugin {
    type = genmon
    Config {
        Command  = /usr/bin/mymonitor.sh  # Command to run
        Interval = 2000                    # Update interval in milliseconds
        Tooltip  = false                   # Use command output as tooltip
    }
}
```

### `alsa` — ALSA Volume Control

Replaces the deprecated OSS `volume` plugin.  Requires `libasound2`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `Card` | string | `default` | ALSA card name passed to `snd_mixer_attach()` |
| `Control` | string | `Master` | Mixer element name (e.g. `Master`, `PCM`) |

```
Plugin {
    type = alsa
    Config {
        Card    = default
        Control = Master
    }
}
```

Left-click toggles the slider popup.  Middle-click toggles mute.
Scroll wheel adjusts by ±2%.  Polls every 500 ms for external changes.

### `deskno` / `deskno2` — Desktop Number

```
Plugin {
    type = deskno
    Config {
        BoldFont  = true
        ShowName  = false       # Show desktop name instead of number
    }
}
```

### `icons` — Window Icons

```
Plugin {
    type = icons
    Config {
        TasksAll  = false       # Show icons from all desktops
    }
}
```

### `image` — Static Image

```
Plugin {
    type = image
    Config {
        image  = /path/to/image.png   # File path or icon name
        action =                       # Command to run on click
    }
}
```

### `separator` — Separator Line

```
Plugin {
    type = separator
    # No config keys
}
```

### `space` — Expanding Spacer

```
Plugin {
    type  = space
    expand = true
    Config {
        size = 2               # Fixed size in pixels (0 = expand)
    }
}
```

### `wincmd` — Window Commands

```
Plugin {
    type = wincmd
    Config {
        Button1   = iconify     # Left click action: iconify | shade | close
        Button3   = none        # Right click action
        image     = window      # Icon to display
    }
}
```

### `user` — User Name

```
Plugin {
    type = user
    Config {
        ShowHostname = false    # Append @hostname to username
    }
}
```

### `windowtitle` — Active Window Title

Displays the title of the currently focused window.  Reads
`_NET_ACTIVE_WINDOW` then `_NET_WM_NAME` (falling back to `WM_NAME`)
via EWMH.  Shows `--` when no window is focused.  Use `expand = true`
so it fills available panel space.  Soft-disable: never — always loads.

```
Plugin {
    type = windowtitle
    expand = true
    Config {
        MaxWidth = 40   # Max chars before truncation with ellipsis (0 = no limit)
    }
}
```

### `loadavg` — System Load Average

Displays 1-minute, 5-minute, and/or 15-minute load averages from
`/proc/loadavg`.  Soft-disables if `/proc/loadavg` is unreadable
(e.g. inside a container without a `/proc` bind-mount).

```
Plugin {
    type = loadavg
    Config {
        Show1  = true   # Show 1-minute load average
        Show5  = true   # Show 5-minute load average
        Show15 = false  # Show 15-minute load average
        Period = 5000   # Update interval in milliseconds (min: 500)
    }
}
```

### `swap` — Swap Usage

Displays swap space usage as a `GtkProgressBar`.  Reads `SwapTotal` and
`SwapFree` from `/proc/meminfo`.  Soft-disables if `/proc/meminfo` is
unreadable; hides automatically when `HideIfNoSwap = true` (default)
and swap size is zero.

```
Plugin {
    type = swap
    Config {
        HideIfNoSwap = true   # Hide widget entirely when no swap exists
        Period = 10000        # Update interval in milliseconds (min: 1000)
    }
}
```

### `cpufreq` — CPU Frequency

Displays the current CPU clock frequency as a text label (e.g. `3.40 GHz`
or `800 MHz`).  Reads `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`.
Soft-disables if the sysfs node is absent (VM, container, or CPU without
frequency scaling).

```
Plugin {
    type = cpufreq
    Config {
        CpuIndex = 0     # CPU core index (0 = cpu0)
        Period   = 2000  # Update interval in milliseconds (min: 250)
    }
}
```

### `thermal` — CPU / Board Temperature

Displays temperature from `/sys/class/thermal/thermal_zoneN/temp` as a
text label (e.g. `45°C`).  Colour-coded: orange at `WarnTemp`, red at
`CritTemp`.  Soft-disables if the thermal zone does not exist.

```
Plugin {
    type = thermal
    Config {
        ThermalZone = 0    # Thermal zone index (0 = thermal_zone0)
        WarnTemp    = 70   # Orange threshold in °C
        CritTemp    = 90   # Red threshold in °C
        Period      = 5000 # Update interval in milliseconds (min: 500)
    }
}
```

### `diskspace` — Filesystem Usage

Displays filesystem usage as a `GtkProgressBar` using `statvfs(3)`.
The tooltip shows absolute used / free / total sizes.  Soft-disables if
the configured mount point is inaccessible at startup.

```
Plugin {
    type = diskspace
    Config {
        MountPoint = /      # Filesystem path to monitor
        Period     = 10000  # Update interval in milliseconds (min: 1000)
    }
}
```

### `diskio` — Disk I/O Throughput

Displays disk read and write throughput as a scrolling bar chart.
Uses the shared `chart` plugin backend (same as `cpu` and `net`).
Reads `/proc/diskstats`.  Soft-disables if the configured device is
not found in `/proc/diskstats`.

```
Plugin {
    type = diskio
    Config {
        Device     = sda      # Block device name to monitor
        ReadLimit  = 100000   # Max read throughput in KiB/s (chart ceiling)
        WriteLimit = 100000   # Max write throughput in KiB/s
        ReadColor  = green    # Chart colour for reads
        WriteColor = red      # Chart colour for writes
    }
}
```

### `brightness` — Backlight Brightness

Displays backlight brightness as a text percentage (e.g. `75%`).
Scroll the mouse wheel up/down over the label to increase/decrease
brightness.  Reads `/sys/class/backlight/<dev>/brightness` and
`max_brightness`.  Auto-detects the first device under
`/sys/class/backlight/` when `Device` is not set.  Soft-disables if
no backlight device is found.

Adjusting brightness requires write access to the brightness sysfs node
(typically granted via a udev `TAG+="uaccess"` rule or membership in
the `video` group).

```
Plugin {
    type = brightness
    Config {
        Device = intel_backlight  # Backlight device name (default: auto-detect)
        Step   = 5                # Adjustment step as % of max (1–50)
        Period = 2000             # Update interval in milliseconds (min: 500)
    }
}
```

---

## Example complete config

```
Global {
    edge        = bottom
    align       = center
    widthtype   = percent
    width       = 100
    height      = 28
    transparent = false
    setdocktype = true
    setpartialstrut = true
    autohide    = false
    iconsize    = 24
}

Plugin {
    type = menu
    Config {
        image = start-here
        system = true
    }
}

Plugin {
    type = launchbar
    Config {
        Button {
            image   = xterm
            tooltip = Terminal
            action  = xterm
        }
    }
}

Plugin {
    type = taskbar
    expand = true
    Config {
        MaxTaskWidth = 150
        IconsOnly = false
    }
}

Plugin {
    type = tray
}

Plugin {
    type = cpu
    Config {
        Color = #00AA00
        Width = 40
    }
}

Plugin {
    type = dclock
    Config {
        ClockFmt = %R
        ShowCalendar = true
    }
}
```

### `xrandr` — Display Resolution

Shows the current resolution of the panel's monitor (e.g. "1920x1080").
Updates automatically when the screen configuration changes.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `Command` | string | (none) | Shell command run on left-click (e.g. `arandr`) |

```
Plugin {
    type = xrandr
    Config {
        Command = arandr
    }
}
```

---

### `xkill` — Window Killer

Click the panel button to enter kill mode (cursor changes to a skull).
Then click any window to send `XKillClient()` for that client.
Right-click or any non-left-click cancels.

No configuration keys.

```
Plugin {
    type = xkill
}
```

---

### `timer` — Countdown Timer

A configurable countdown timer.  Left-click starts the timer; another click
resets it.  On expiry the label flashes "DONE" until clicked.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `Duration` | int | `300` | Countdown duration in seconds (1..86400) |

```
Plugin {
    type = timer
    Config {
        Duration = 300
    }
}
```

---

### `clipboard` — Clipboard History

Monitors the X11 CLIPBOARD selection.  Each time the owner changes and
the content is text, it is stored in a history ring buffer.  Left-click
pops up a menu; selecting an item restores it to the clipboard.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `MaxHistory` | int | `10` | Maximum number of entries to keep (1..100) |
| `WatchPrimary` | bool | `false` | Also monitor the PRIMARY selection |

```
Plugin {
    type = clipboard
    Config {
        MaxHistory   = 10
        WatchPrimary = false
    }
}
```

---

### `windowlist` — Window List

A compact button that pops up a menu listing all open windows (from
`_NET_CLIENT_LIST`).  Selecting an item raises and focuses that window.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `MaxTitle` | int | `50` | Max characters of title per menu item (0 = no limit) |

```
Plugin {
    type = windowlist
    Config {
        MaxTitle = 50
    }
}
```

---

### `capslock` — Lock Key Indicators

Shows the state of Caps Lock, Num Lock, and/or Scroll Lock as coloured
text labels.  Active locks are shown in bold; inactive ones are dimmed
(or hidden when `HideInactive` is enabled).  Polls via XKB every 200 ms.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `ShowCaps` | bool | `true` | Show Caps Lock indicator ("A") |
| `ShowNum` | bool | `true` | Show Num Lock indicator ("1") |
| `ShowScroll` | bool | `false` | Show Scroll Lock indicator ("S") |
| `HideInactive` | bool | `false` | Hide inactive indicators entirely |

```
Plugin {
    type = capslock
    Config {
        ShowCaps     = true
        ShowNum      = true
        ShowScroll   = false
        HideInactive = false
    }
}
```

---

### `kbdlayout` — Keyboard Layout

Shows the short name of the currently active XKB keyboard group (e.g.
"us", "de", "fr").  Left-click cycles to the next layout; right-click
shows a menu of all configured layouts.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `Period` | int | `500` | Poll interval in milliseconds (min: 100) |

```
Plugin {
    type = kbdlayout
    Config {
        Period = 500
    }
}
```

---

## Reloading configuration

Send `SIGUSR1` to the fbpanel process to reload the configuration:

```bash
killall -USR1 fbpanel
```

Alternatively, use the "Configure Panel" dialog (right-click on the panel).
