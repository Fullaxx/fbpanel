# fbpanel — Memory Model and Ownership

This document describes how memory is managed throughout fbpanel, who
owns each object, and what must be freed in destructors.

---

## Ownership principles

fbpanel follows GTK2 / GLib memory conventions:

1. **GObject ref-counting**: GtkWidget and other GObject subclasses are
   reference-counted. `g_object_ref()` / `g_object_unref()` control the
   lifetime. `gtk_widget_destroy()` removes a widget from the screen and
   drops the container's reference; if no other ref exists the object is
   freed.

2. **Caller owns returned strings**: Functions returning `gchar *` (GLib
   string allocation) transfer ownership to the caller, who must call
   `g_free()`. Functions returning `const gchar *` do **not** transfer
   ownership.

3. **Container owns children**: When a widget is added to a GTK container,
   the container holds a reference. The parent is responsible for destroying
   children (usually via `gtk_widget_destroy()` on itself which propagates).

4. **Timers are not automatic**: A `g_timeout_add()` timer fires until the
   callback returns `FALSE` or `g_source_remove()` is called. Timers hold
   a GLib reference to their callback data and will call back into freed
   memory if not removed before destruction.

5. **Signal connections are not automatic**: `g_signal_connect()` connections
   persist until explicitly disconnected with `g_signal_handler_disconnect()`
   or `g_signal_handlers_disconnect_by_func()`. Stale signal connections to
   destroyed objects will crash.

---

## Plugin memory lifecycle

```
panel  ──owns──►  plugin_instance  ──owns──►  pwid (GtkBgbox)
                      │                           │
                      ▼                           ▼
                  priv data                  child widgets
                  (g_malloc)               (owned by pwid)
```

### Allocation (plugin_load)

```c
plugin_instance *p = g_malloc0(class->priv_size);
p->class  = class;
p->panel  = panel;
p->xc     = xc_subtree;          // NOT owned by plugin
p->pwid   = gtk_bgbox_new();     // plugin owns this ref
gtk_widget_show(p->pwid);
// then calls class->constructor(p)
```

The plugin's `constructor` receives `p` with `pwid` already created.
The plugin adds its own child widgets into `pwid`.

### Destruction (plugin_stop)

```c
class->destructor(p);            // plugin cleans up its own resources
gtk_widget_destroy(p->pwid);     // destroys pwid and all child widgets
g_free(p);                       // frees the plugin_instance + priv data
```

**The plugin destructor must NOT destroy `p->pwid`** — that is done by
the panel after the destructor returns.

---

## Plugin destructor checklist

Every plugin destructor must clean up exactly the resources its constructor
created. In order:

### 1. Remove all timers

```c
// constructor saved: priv->timer_id = g_timeout_add(interval, cb, priv);
// destructor must:
if (priv->timer_id) {
    g_source_remove(priv->timer_id);
    priv->timer_id = 0;
}
```

Failing to remove a timer is a **use-after-free bug**: the timer fires
after `g_free(p)` and writes into freed memory.

### 2. Disconnect GObject signal handlers

```c
// connected in constructor:
g_signal_connect(G_OBJECT(fbev), "active_window", G_CALLBACK(cb), priv);
// disconnect in destructor:
g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), G_CALLBACK(cb), priv);
```

The panel's `fbev` object outlives all plugins, so stale signal
connections to `fbev` will fire into freed memory.

### 3. Close file descriptors

Plugins that open `/proc/stat`, `/sys/class/power_supply/`, ALSA handles,
or other file descriptors must close them in the destructor.

### 4. Free allocated strings and arrays

```c
g_free(priv->config_string);     // strings read from xconf
g_strfreev(priv->argv);          // NULL-terminated string arrays
```

### 5. Release helper plugin references

Plugins that use helper plugins (`meter`, `chart`) must release the
reference:
```c
class_put("meter");   // paired with class_get("meter") in constructor
```

### 6. Do NOT destroy pwid

```c
// WRONG — panel does this:
// gtk_widget_destroy(p->pwid);

// CORRECT — just clean up your own data
```

---

## GtkBgbox widget ownership

The panel creates a `GtkBgbox` for each plugin's `pwid`. Ownership
flows as follows:

```
panel->box (GtkHBox)
    └── pwid (GtkBgbox)          ref held by panel->box
            └── plugin_widget    ref held by pwid
```

When `gtk_widget_destroy(pwid)` is called:
- `pwid` is destroyed and removed from `panel->box`
- All children of `pwid` are recursively destroyed
- All GObject signal connections on those widgets are disconnected

---

## FbBg (background pixmap) lifetime

`FbBg` is a reference-counted GObject:

```c
FbBg *bg = fb_bg_get_for_display();   // increments ref count
// ... use bg ...
g_object_unref(bg);                    // decrements; freed when count hits 0
```

`GtkBgbox` holds a reference to `FbBg` while it has a root-pixmap
background. When the `GtkBgbox` is destroyed, it unrefs `FbBg`.

The `FbBg` singleton monitors the `_XROOTPMAP_ID` property and emits a
`changed` signal when the wallpaper changes. `GtkBgbox` connects to this
signal and queues a redraw; it must disconnect in its `finalize` vfunc.

---

## X11 property memory

The `get_xaproperty()` family of functions return data allocated by
`XGetWindowProperty()`. The caller must free it with `XFree()`:

```c
guchar *data = NULL;
int count = get_xaproperty(win, atom, type, &data);
// use data ...
XFree(data);
```

`get_utf8_property()` and `get_textproperty()` return a `gchar *`
allocated with `g_malloc` / `g_strdup`. The caller must `g_free()` it.

`get_utf8_property_list()` returns a `gchar **` (NULL-terminated).
Free with `g_strfreev()`.

---

## xconf tree ownership

The `xconf` config tree is owned by the panel. Each plugin receives a
pointer (`p->xc`) into the panel's tree — the plugin does **not** own
this pointer and must not free it. Config values read with `XCG()` into
`gchar *` fields are owned by the xconf tree node; do not free them
unless the xconf API explicitly transfers ownership.

---

## GModule (plugin .so) lifetime

Plugin `.so` files are loaded with `g_module_open()`. The `GModule`
handle is stored in the plugin's `class` (via the registry). The module
is **not** unloaded until the panel exits (`g_module_close()`). This
means static data inside a plugin `.so` persists for the process lifetime.

---

## Icon theme references

`icon_theme` is a process-global `GtkIconTheme *`. Do **not** unref it.
Plugins that cache pixbufs loaded from the icon theme should unref those
pixbufs when no longer needed:

```c
// Load (caller gets a new ref):
GdkPixbuf *pb = gtk_icon_theme_load_icon(icon_theme, name, size, 0, NULL);
// ... use pb ...
g_object_unref(pb);   // release when done
```

---

## Common memory bugs to watch for

| Pattern | Bug | Fix |
|---------|-----|-----|
| Timer ID not saved | Can't remove timer in destructor → use-after-free | Store return value of `g_timeout_add()` |
| Signal not disconnected | Callback fires after struct freed | `g_signal_handlers_disconnect_by_func()` |
| `XFree()` vs `g_free()` | Wrong deallocator crashes | Use `XFree()` for X11, `g_free()` for GLib |
| Missing `g_object_unref()` | Pixbuf / GObject leak | Unref every owned reference |
| `xc` pointer freed | Corrupts panel config tree | Never free `p->xc` |
| `pwid` destroyed in destructor | Double-destroy → crash | Let panel destroy `pwid` |
