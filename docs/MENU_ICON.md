# Menu button icon

The small clickable icon at the edge of the panel (the "start menu" button) is
produced by the `menu` plugin.  Its appearance is controlled entirely by two
keys in your fbpanel config file — there is no auto-detection of the running
distro.

## Config keys

Inside the `menu` plugin's `config` block (`~/.config/fbpanel/default`):

```
Plugin {
    type = menu
    config {
        icon  = logo        # name to look up in the active GTK icon theme
        #image = ~/images/mylogo.png   # or load a specific file
        ...
    }
}
```

| Key | What it does |
|-----|-------------|
| `icon = <name>` | Ask the current GTK 2 icon theme for an icon by that name |
| `image = <path>` | Load an image file directly (`~` is expanded) |

Both keys are optional.  If neither is set the button is created without any
image (see BUG note in `menu.c:make_button`).

## Resolution order

`panel/fbwidgets.c:fb_pixbuf_new()` tries sources in this order, stopping at
the first success:

1. **GTK icon theme lookup** — `gtk_icon_theme_load_icon(icon_theme, iname, ...)`
2. **File load** — `gdk_pixbuf_new_from_file_at_size(fname, ...)`
3. **`gtk-missing-image`** placeholder (only when `use_fallback` is `TRUE`)

For the menu button, `icon` (iname) is tried before `image` (fname).

## Why you might see different icons

### Debian / Ubuntu swirl

The shipped default config (`data/config/default.in`) sets:

```
icon = logo
```

On Debian and Ubuntu the active GTK theme ships an icon named `logo` that
renders as the distro swirl.  If `logo` resolves successfully in your current
theme, that is what you see.

### Star

Two situations produce a star:

1. **`icon = star`** is set explicitly (used in some example configs).  Many
   themes ship a generic star-shaped icon under that name.

2. **`logo` is not found** in the current theme and the config also has an
   `image` fallback pointing to a star PNG (the old www example config
   commented out `/tmp/usr/share/fbpanel/images/star.png` for exactly this
   purpose).

## Checking what your config currently uses

```bash
grep -A 10 'type = menu' ~/.config/fbpanel/default
```

## Checking whether your theme provides a given icon name

```bash
# GTK 2 — quick Python check
python2 -c "
import gtk
t = gtk.icon_theme_get_default()
print t.lookup_icon('logo', 22, 0)
"
```

A `None` result means the theme has no icon by that name and fbpanel will
move on to the `image` fallback (or show nothing if that is also absent).

## Changing the icon

Edit `~/.config/fbpanel/default` and restart fbpanel (or send `SIGUSR1` to
reload):

```
# Use a theme icon:
icon = start-here

# Use a custom file:
image = ~/.config/fbpanel/mylogo.png

# Both set: icon is tried first, image is the fallback
icon  = start-here
image = ~/.config/fbpanel/mylogo.png
```

Common theme icon names that work on most systems: `start-here`, `logo`,
`star`, `system-logo-symbolic`, `distributor-logo`.
