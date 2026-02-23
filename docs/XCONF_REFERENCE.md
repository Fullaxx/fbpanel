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

### `volume` — ALSA Volume Control

```
Plugin {
    type = volume
    Config {
        Card    = default       # ALSA card name
        Selem   = Master        # ALSA simple element name
        Mute    = true          # Allow mute toggle on click
        Step    = 5             # Volume step for scroll wheel (percent)
    }
}
```

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

---

## Reloading configuration

Send `SIGUSR1` to the fbpanel process to reload the configuration:

```bash
killall -USR1 fbpanel
```

Alternatively, use the "Configure Panel" dialog (right-click on the panel).
