# fbpanel — Using fbpanel Components as a Library

fbpanel is primarily an application binary, not a development library.
However, its modular architecture means several components can be
studied and adapted. This document describes how the internal APIs
are structured for developers who want to embed fbpanel behaviour
or write complex plugins.

---

## Plugin as a library consumer

The most common "library" use case is writing a plugin. A plugin gets
access to the entire fbpanel internal API through `plugin.h`, which
transitively includes `panel.h` and `misc.h`.

```c
#include "plugin.h"
/* This gives you:
 *   plugin_class, plugin_instance  — plugin contract
 *   panel, the_panel               — global panel state
 *   fbev, FbEv                     — EWMH event bus
 *   icon_theme                     — global GtkIconTheme
 *   Atom a_NET_*, a_WM_*           — interned X11 atoms
 *   GDK_DPY                        — X11 Display* macro
 *   XCG(), xconf_find()            — config reading
 *   fb_pixbuf_new(), fb_image_new(), fb_button_new()  — widget factories
 *   get_xaproperty(), Xclimsg()    — X11 helpers
 *   run_app()                      — command execution
 */
```

---

## Accessing the panel struct

The global `the_panel` gives full access to panel state:

```c
panel *p = the_panel;

// Panel dimensions
int panel_width  = p->cw;        // current panel width
int panel_height = p->ch;        // current panel height
int panel_edge   = p->edge;      // EDGE_TOP / EDGE_BOTTOM / EDGE_LEFT / EDGE_RIGHT

// Panel GTK window
GtkWidget *topwin = p->topgwin;  // the GtkWindow
GdkWindow *gdkwin = topwin->window;  // the GdkWindow

// Plugin list
GSList *plugins = p->plugins;    // GSList of plugin_instance*
```

**Caution:** Modifying `the_panel` fields directly from a plugin is
unsupported and may cause crashes. Treat `the_panel` as read-only.

---

## X11 helper API (misc.h)

### `GDK_DPY` macro

```c
Display *dpy = GDK_DPY;
```

Equivalent to `GDK_DISPLAY_XDISPLAY(gdk_display_get_default())`.
Use this whenever a raw `Display *` is needed for X11 calls.

### Reading X11 properties

```c
/* Generic property reader — returns number of items, fills *ret */
int get_xaproperty(Window win, Atom prop, Atom type, guchar **ret);

/* Convenience wrappers: */
gchar  *get_utf8_property(Window win, Atom atom);
gchar **get_utf8_property_list(Window win, Atom atom, int *count);
gchar  *get_textproperty(Window win, Atom atom);
```

All returned strings/arrays must be freed by the caller:
- `gchar *` → `g_free()`
- `gchar **` → `g_strfreev()`
- raw `guchar *` from `get_xaproperty()` → `XFree()`

### Sending X11 client messages

```c
/* Send a _NET_* message to a window */
void Xclimsg(Window win, Atom type, long l0, long l1, long l2, long l3, long l4);

/* Send a WM protocol message (e.g., WM_DELETE_WINDOW) */
void Xclimsgwm(Window win, Atom type, Atom arg);
```

### EWMH helpers

```c
int  get_net_current_desktop(void);
int  get_net_wm_desktop(Window win);
int  get_net_number_of_desktops(void);
/* Returns a bitfield: */
net_wm_state       get_net_wm_state(Window win);
net_wm_window_type get_net_wm_window_type(Window win);
```

---

## Pixbuf and widget factory API (misc.h)

### `fb_pixbuf_new()`

```c
/*
 * Load a pixbuf from icon name and/or file path.
 * icon:  icon theme name (e.g. "terminal"), or NULL
 * file:  absolute or ~ path to image file, or NULL
 * size:  icon size in pixels (used for theme lookup)
 * Returns: new GdkPixbuf (caller owns ref), or NULL on failure.
 */
GdkPixbuf *fb_pixbuf_new(const gchar *icon, const gchar *file, int size);
```

### `fb_image_new()`

```c
/*
 * Create a GtkImage that auto-reloads when the icon theme changes.
 * icon, file, size: same as fb_pixbuf_new()
 * Returns: new GtkImage widget (floating ref — add to container).
 */
GtkWidget *fb_image_new(const gchar *icon, const gchar *file, int size);
```

### `fb_button_new()`

```c
/*
 * Create a pressable icon button in a GtkBgbox.
 * Adds hover highlight and press animation.
 * icon, file, size: same as fb_pixbuf_new()
 * Returns: GtkBgbox containing the image (floating ref).
 */
GtkWidget *fb_button_new(const gchar *icon, const gchar *file, int size);
```

### `fb_create_calendar()`

```c
/*
 * Create and show a floating calendar window.
 * Positions itself near (x, y) on screen.
 * Caller does not own the window — it destroys itself on focus-out.
 */
void fb_create_calendar(int x, int y);
```

---

## Config reading API (xconf.h)

```c
/* XCG macro: read a typed value from config subtree xc */
XCG(xc, "key", &variable, int);         /* integer */
XCG(xc, "key", &variable, str);         /* gchar* (NOT caller-owned) */
XCG(xc, "key", &variable, enum, table); /* int via string lookup */

/* Direct lookup: */
xconf *xconf_find(xconf *xc, const gchar *key);
gchar *xconf_find_str(xconf *xc, const gchar *key);
int    xconf_find_int(xconf *xc, const gchar *key, int def);
```

---

## Command execution API (run.h)

```c
/*
 * Run a shell command asynchronously.
 * Shows an error dialog if spawn fails.
 */
void run_app(const gchar *cmd);

/*
 * Run a command from an argv array.
 * args: NULL-terminated array of strings.
 */
void run_app_argv(gchar **args);
```

---

## Chart widget API (plugins/chart/chart.h)

The `chart` plugin exports a reusable scrolling bar-graph widget.
Other plugins (`cpu`, `mem`, `net`) use it:

```c
#include "chart.h"

chart *chart_new(void);
/* chart->widget is the GtkDrawingArea — add it to your pwid */

void chart_set_range(chart *c, double min, double max);
void chart_set_color(chart *c, GdkColor *color);
void chart_push_value(chart *c, double value);  /* add new sample, scroll left */
void chart_free(chart *c);   /* call in destructor before pwid is destroyed */
```

---

## Meter widget API (plugins/meter/meter.h)

The `meter` plugin exports an icon-based level meter:

```c
#include "meter.h"

/* Get and use the meter class: */
plugin_class *mc = class_get("meter");
plugin_instance *meter = plugin_load(mc, panel, xc);

/* Set the level (0–100): */
meter_set_level(meter, 75);

/* Teardown: */
plugin_put(meter);
class_put("meter");
```

---

## GtkBgbox API (panel/gtkbgbox.h)

Used as the `pwid` container for every plugin. Can also be used
as a styled container inside a plugin:

```c
#include "gtkbgbox.h"

GtkWidget *box = gtk_bgbox_new();

/* Background modes: */
gtk_bgbox_set_background(box, BG_NONE, NULL, 0);          /* no bg */
gtk_bgbox_set_background(box, BG_STYLE, NULL, 0);         /* GTK theme */
gtk_bgbox_set_background(box, BG_ROOT, &color, alpha);    /* root pixmap */
gtk_bgbox_set_background(box, BG_INHERIT, &color, alpha); /* copy parent */
```

---

## Internationalisation

Mark strings for translation using the `c_()` macro (defined in `panel.h`):

```c
const gchar *label = c_("My Plugin");
```

This expands to `gettext("My Plugin")`. Translations live in `po/*.po`
and are compiled to `po/*.mo` by the build system.

---

## Thread safety

**fbpanel is entirely single-threaded.** All GTK calls, X11 calls,
timer callbacks, and signal handlers run on the main thread in the
GTK main loop. Do not call GTK functions from background threads.

If a plugin needs to do I/O or computation off-thread, use GLib's
`GAsyncQueue`, `GThread` + `gdk_threads_enter/leave()`, or simply
use `g_idle_add()` to post work back to the main thread.
