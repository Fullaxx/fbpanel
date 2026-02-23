# fbpanel — GTK2 Widget Lifecycle

This document describes how GTK2 widgets are created, shown, and
destroyed within fbpanel, and the specific responsibilities of
plugin code at each lifecycle phase.

---

## GTK2 widget lifecycle overview

```
gtk_widget_new() / gtk_FOO_new()
        │
        ▼
   [realize]          Widget gets an X11 GdkWindow (maps to X window)
        │              Triggered by gtk_widget_realize() or first show
        ▼
   [map / show]       Widget becomes visible on screen
        │              gtk_widget_show() / gtk_widget_show_all()
        ▼
   [expose-event]     Widget paints itself (GDK/Cairo)
        │              Triggered by damage, resize, or explicit queue
        ▼
   [unrealize]        X11 GdkWindow destroyed (widget object still alive)
        │
        ▼
   [destroy]          Widget receives destroy signal; cleans up internals
        │              gtk_widget_destroy() triggers this
        ▼
   [finalize]         GObject reference count hits 0; memory freed
                      g_object_unref() (or last container unref)
```

---

## The panel GtkWindow lifecycle

The top-level panel window is created in `panel_start_gui()`:

```c
p->topgwin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
gtk_window_set_wmclass(GTK_WINDOW(p->topgwin), "panel", "fbpanel");
gtk_window_set_type_hint(GTK_WINDOW(p->topgwin),
    GDK_WINDOW_TYPE_HINT_DOCK);
// ... set geometry, struts, etc.
gtk_widget_show_all(p->topgwin);
```

The window is realized during `gtk_widget_show_all()`, at which point
the X11 window is created and struts/type properties are applied.

On exit, `gtk_widget_destroy(p->topgwin)` recursively destroys all
child widgets (the panel hbox and all plugin `pwid` widgets).

---

## Plugin widget lifecycle

### Phase 1: pwid creation (before constructor)

`plugin_load()` creates `p->pwid` before calling the constructor:

```c
p->pwid = gtk_bgbox_new();
gtk_widget_show(p->pwid);
gtk_box_pack_start(GTK_BOX(panel->box), p->pwid, FALSE, TRUE, 0);
```

At this point `p->pwid` is:
- Created but not yet realized (no X window yet)
- Added to the panel hbox (container holds a ref)
- Shown (`GTK_WIDGET_VISIBLE` flag set)

### Phase 2: constructor

The plugin constructor adds its own content into `p->pwid`:

```c
static int my_constructor(plugin_instance *p) {
    my_priv *priv = (my_priv *) p;

    // Create a child widget and add it to pwid
    priv->label = gtk_label_new("hello");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    // Start a periodic timer
    priv->timer_id = g_timeout_add(1000, my_update, priv);

    return 1;   // 1 = success, 0 = failure
}
```

Rules:
- Must return `1` on success, `0` on failure.
- Must **not** call `gtk_widget_destroy(p->pwid)` (even on failure
  the panel cleans up `pwid`).
- Should call `gtk_widget_show()` on all newly created child widgets.

### Phase 3: realize (automatic)

When the panel window is shown, `gtk_widget_show_all()` propagates
and eventually realizes `p->pwid` and all its children. Plugins do not
need to handle this manually unless they have special X11 setup that
requires a realized window (e.g., XEMBED — see tray plugin).

### Phase 4: expose / draw (automatic)

GTK2 uses `expose_event` to repaint. `GtkBgbox` overrides this to
paint the pseudo-transparent background before delegating to children.
Plugins using `GtkDrawingArea` (chart, tclock, pager) connect to
`expose_event` and draw with GDK/Cairo.

```c
g_signal_connect(G_OBJECT(da), "expose_event",
    G_CALLBACK(my_draw), priv);
```

### Phase 5: destructor

Called by `plugin_stop()` before `gtk_widget_destroy(p->pwid)`:

```c
static void my_destructor(plugin_instance *p) {
    my_priv *priv = (my_priv *) p;

    // 1. Remove timer
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
    }

    // 2. Disconnect signals on objects that outlive pwid
    g_signal_handlers_disconnect_by_func(G_OBJECT(fbev),
        G_CALLBACK(my_event_cb), priv);

    // 3. Free allocated data
    g_free(priv->config_string);

    // 4. Do NOT destroy p->pwid — panel does this
}
```

### Phase 6: pwid destruction (after destructor)

```c
gtk_widget_destroy(p->pwid);   // destroys pwid + all child widgets
g_free(p);                      // frees plugin_instance + priv data
```

The `destroy` signal propagates to all children of `pwid`.
`GtkBgbox::destroy` unrefs `FbBg`, closes any signal connections on
`FbBg`, and calls the parent `GtkEventBox::destroy`.

---

## GtkBgbox-specific lifecycle

`GtkBgbox` has additional lifecycle hooks for the background system:

| Event | What happens |
|-------|-------------|
| `realize` | Calls `gdk_window_set_back_pixmap(NULL, TRUE)` to disable the default GTK background fill; background is handled entirely by `expose_event` |
| `expose_event` | Copies the root window region (via `FbBg`) into the widget's GdkWindow, then chains to parent's expose handler |
| `style_set` | Re-applies the background when the GTK theme changes |
| `destroy` | Unrefs `FbBg`; disconnects from `FbBg::changed` signal |

---

## XEMBED lifecycle (tray plugin)

The tray plugin has the most complex widget lifecycle due to XEMBED:

1. `tray_constructor()` creates an `EggTrayManager` and calls
   `egg_tray_manager_manage_screen()` — this selects the
   `_NET_SYSTEM_TRAY_S<N>` selection and starts listening for docking
   requests.

2. When a tray application connects, `EggTrayManager` emits `tray_icon_added`.
   The tray plugin creates a `GtkSocket` for it:
   ```c
   socket = gtk_socket_new();
   gtk_container_add(GTK_CONTAINER(p->pwid), socket);
   gtk_widget_show(socket);
   egg_tray_manager_child_message(mgr, icon, socket);
   ```

3. The `GtkSocket` realizes and gets an X11 window. The tray application
   embeds its window into this socket via XEMBED.

4. When the tray icon exits, `tray_icon_removed` is emitted. The socket
   must be destroyed:
   ```c
   gtk_widget_destroy(socket);
   ```

5. When the tray plugin is destroyed, `EggTrayManager` releases the
   selection, causing all embedded applications to be un-embedded.

---

## Timer management

Timers (`g_timeout_add`) are integrated with the GTK main loop and run
on the main thread. They are safe to call GTK functions from.

**Critical rules:**

1. Always save the timer source ID:
   ```c
   priv->timer_id = g_timeout_add(interval_ms, callback, priv);
   ```

2. Always remove the timer in the destructor:
   ```c
   if (priv->timer_id) {
       g_source_remove(priv->timer_id);
       priv->timer_id = 0;
   }
   ```

3. A callback that returns `FALSE` removes itself — do not also call
   `g_source_remove()` on it (double-remove is a bug).

4. A callback that returns `TRUE` continues firing — it will fire into
   freed memory if not removed.

---

## Signal connection rules

| Object | Connected in | Must disconnect in |
|--------|-------------|-------------------|
| `fbev` signals | constructor | destructor (fbev outlives plugins) |
| Child widget signals (`priv->label`, etc.) | constructor | NOT needed — destroyed with `pwid` |
| `icon_theme "changed"` | constructor | destructor (`icon_theme` is global) |
| `FbBg "changed"` | `GtkBgbox` realize | `GtkBgbox` destroy (internal) |

**Rule:** Only disconnect from objects that outlive `p->pwid`. Signals
connected to widgets inside `pwid` are cleaned up automatically when
`pwid` is destroyed.

---

## Autohide window behaviour

When autohide is enabled:
- The panel window sets its size to 1px (barely visible) and starts a
  hide timer (`panel->hide_timeout`).
- On mouse-enter, the timer is cancelled and the window expands to full
  size via `gtk_window_resize()`.
- On mouse-leave, the hide timer restarts.

The autohide state machine uses GTK2 `enter-notify-event` and
`leave-notify-event` signals on the top-level window.

---

## GTK2-specific considerations

fbpanel targets GTK 2.0 exclusively. Key GTK2 differences from GTK3:

| Feature | GTK2 | GTK3 |
|---------|------|------|
| Drawing | `expose_event` signal + GDK drawing | `draw` signal + Cairo |
| Backgrounds | `gdk_window_set_back_pixmap()` | CSS / `cairo_t` |
| Widget styles | `GtkStyle` / `gtk_widget_modify_*()` | CSS |
| `GdkPixmap` | Used for offscreen drawing | Replaced by `cairo_surface_t` |
| `GdkColor` | 16-bit RGB | `GdkRGBA` (float) |
| `GtkObject` base | Exists | Removed (GObject only) |

Many GTK2 APIs used in fbpanel are deprecated in GTK3. This is
intentional — fbpanel (GTK2) is a separate branch from fbpanel3 (GTK3).
Do not apply GTK3 deprecation fixes to this codebase.
