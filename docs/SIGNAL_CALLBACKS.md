# Signal / Callback Signature Reference

The single most recurring bug class in fbpanel's history is a callback wired
up with the wrong signature for what its signal, timer, or event source
actually dispatches — GTK/GDK don't type-check `GCallback` at compile time,
so a mismatched parameter count or type compiles cleanly and only crashes
(or silently misbehaves) at runtime. [BUGS_AND_ISSUES.md](BUGS_AND_ISSUES.md)
tracks four confirmed instances of exactly this (BUG-010, BUG-017, BUG-018,
BUG-019) — see the worked examples in each section below.

This page is a **reference table**, not a call-site log: with ~200 signal/
timer/filter registrations across the codebase, a line-by-line log would go
stale the moment anyone touches the code. Instead, look up the canonical
signature for whatever you're connecting, and cross-check your callback
against it. The closing "Auditing for regressions" section gives the grep
command to re-find every registration yourself.

---

## Custom project signals (`g_signal_new`)

Signals this codebase defines itself, via a `GObject` subclass's
`class_init()`. All are `G_TYPE_NONE` return, `void` marshaller — no signal
accumulator, no return-value handling to get wrong.

| Signal | Owner | Callback signature | Defined in |
|---|---|---|---|
| `current_desktop` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `number_of_desktops` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `desktop_names` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `active_window` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `client_list_stacking` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `client_list` | `FbEv` | `void cb(FbEv *ev, gpointer data)` | panel/ev.c |
| `changed` | `FbBg` | `void cb(FbBg *bg, gpointer data)` | panel/bg.c |
| `tray_icon_added` | `EggTrayManager` | `void cb(EggTrayManager *mgr, EggTrayManagerChild *child, gpointer data)` | plugins/tray/eggtraymanager.c |
| `tray_icon_removed` | `EggTrayManager` | `void cb(EggTrayManager *mgr, EggTrayManagerChild *child, gpointer data)` | plugins/tray/eggtraymanager.c |
| `message_sent` | `EggTrayManager` | `void cb(EggTrayManager *mgr, EggTrayManagerChild *child, const gchar *message, glong id, glong timeout, gpointer data)` | plugins/tray/eggtraymanager.c |
| `message_cancelled` | `EggTrayManager` | `void cb(EggTrayManager *mgr, EggTrayManagerChild *child, glong id, gpointer data)` | plugins/tray/eggtraymanager.c |
| `lost_selection` | `EggTrayManager` | `void cb(EggTrayManager *mgr, gpointer data)` | plugins/tray/eggtraymanager.c |

All six `FbEv` signals share one signature shape — easy to remember, easy to
get subtly wrong if you assume one of them secretly carries an argument
(none do; read the new state back with the `fb_ev_*` accessor functions in
[ev.h](../panel/ev.h) instead of expecting it in the callback).

---

## Stock GTK/GDK signals used here

Every distinct signal name this codebase connects to, with its canonical
callback signature and one real call site as a worked example. `-` and `_`
are interchangeable in GTK/GLib signal names (both forms appear below,
matching whichever the call site actually used).

| Signal | Canonical callback signature | Example |
|---|---|---|
| `activate` | `void cb(GtkWidget *widget, gpointer data)` | panel/panel.c:672 |
| `button-press-event` / `button_press_event` | `gboolean cb(GtkWidget *widget, GdkEventButton *event, gpointer data)` | panel/plugin.c:469 |
| `button-release-event` / `button_release_event` | `gboolean cb(GtkWidget *widget, GdkEventButton *event, gpointer data)` | plugins/taskbar/taskbar.c |
| `changed` (GtkIconTheme / GtkComboBox / GtkTreeSelection) | `void cb(GtkWidget *widget, gpointer data)` | plugins/menu/menu.c:677 |
| `clicked` | `void cb(GtkButton *button, gpointer data)` | plugins/deskno/deskno.c:193 |
| `color-set` | `void cb(GtkColorButton *widget, gpointer data)` | panel/gconf.c:409 |
| `configure-event` / `configure_event` | `gboolean cb(GtkWidget *widget, GdkEventConfigure *event, gpointer data)` | panel/panel.c:784 |
| `delete_event` | `gboolean cb(GtkWidget *widget, GdkEvent *event, gpointer data)` | panel/gconf_panel.c:597 |
| `destroy-event` | `gboolean cb(GtkWidget *widget, GdkEvent *event, gpointer data)` | panel/panel.c:776 |
| `drag_data_received` / `drag-data-received` | `void cb(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sd, guint info, guint time, gpointer data)` | plugins/launchbar/launchbar.c:323 |
| `drag-leave` | `void cb(GtkWidget *widget, GdkDragContext *ctx, guint time, gpointer data)` | plugins/taskbar/taskbar.c:1130 |
| `drag-motion` | `gboolean cb(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y, guint time, gpointer data)` | plugins/taskbar/taskbar.c:1128 |
| `enter` / `leave` (GtkButton, pre-GTK2.10 style) | `void cb(GtkButton *button, gpointer data)` | plugins/taskbar/taskbar.c:1122 |
| `enter-notify-event` / `leave-notify-event` | `gboolean cb(GtkWidget *widget, GdkEventCrossing *event, gpointer data)` | panel/fbwidgets.c:632 |
| `expose-event` / `expose_event` | `gboolean cb(GtkWidget *widget, GdkEventExpose *event, gpointer data)` | plugins/tray/fixedtip.c (see BUG-010 below) |
| `map-event` | `gboolean cb(GtkWidget *widget, GdkEvent *event, gpointer data)` | panel/panel.c:782 |
| `plug_removed` | `gboolean cb(GtkSocket *socket, gpointer data)` | plugins/tray/eggtraymanager.c:630 |
| `realize` | `void cb(GtkWidget *widget, gpointer data)` | plugins/tray/eggtraymanager.c:581 |
| `response` | `void cb(GtkDialog *dialog, gint response_id, gpointer data)` | panel/gconf_panel.c:595 |
| `scroll-event` | `gboolean cb(GtkWidget *widget, GdkEventScroll *event, gpointer data)` | panel/panel.c:788 |
| `size-allocate` | `void cb(GtkWidget *widget, GtkAllocation *allocation, gpointer data)` | plugins/tray/main.c:386 |
| `size-changed` | `void cb(GdkScreen *screen, gpointer data)` | plugins/xrandr/xrandr.c (see BUG-018 below) |
| `size-request` | `void cb(GtkWidget *widget, GtkRequisition *requisition, gpointer data)` | panel/panel.c:778 |
| `style_set` / `style-set` | `void cb(GtkWidget *widget, GtkStyle *previous_style, gpointer data)` | plugins/tray/eggtraymanager.c:588 |
| `toggled` | `void cb(GtkToggleButton *widget, gpointer data)` | panel/gconf.c:325 |
| `unmap` | `void cb(GtkWidget *widget, gpointer data)` | plugins/menu/menu.c:390 |
| `value-changed` / `value_changed` | `void cb(GtkRange *widget, gpointer data)` (or `GtkAdjustment *` depending on which object the signal was connected to) | panel/gconf.c:201 |

---

## `GdkFilterFunc` (raw X11 event filters)

Registered via `gdk_window_add_filter()`. Fixed signature, no exceptions:

```c
GdkFilterReturn cb(GdkXEvent *xevent, GdkEvent *event, gpointer data);
```

Used in 7 files today, all with the correct signature (this table doubles as
a regression baseline — if you touch one of these, the signature above is
what to preserve): `panel/panel.c`, `panel/gtkbgbox.c`,
`plugins/tray/eggtraymanager.c`, `plugins/pager/pager.c`,
`plugins/xkill/xkill.c`, `plugins/icons/icons.c`,
`plugins/taskbar/taskbar.c`.

---

## `GSourceFunc` (`g_timeout_add`)

```c
gboolean cb(gpointer data);   // return TRUE to keep firing, FALSE to stop
```

63 call sites. The specific footgun this codebase has already hit once
(BUG-019, below): casting a function of the wrong arity through
`(GSourceFunc)` **compiles silently** — the cast suppresses any prototype
mismatch warning. There is no substitute for checking, by eye, that the
function you're passing actually matches `gboolean f(gpointer)`.

---

## Worked examples: every already-fixed bug in this pattern

### BUG-010 — wrong first-parameter type (`expose-event`)

`plugins/tray/fixedtip.c`'s `expose_handler` was declared
`expose_handler(GtkTooltips *tooltips)` but connected to `"expose_event"`,
which GTK dispatches as `(GtkWidget*, GdkEventExpose*, gpointer)`. Current
(fixed) signature:

```c
static gboolean
expose_handler (GtkWidget *widget, GdkEventExpose *event, gpointer data)
```

### BUG-017 — wrong signal *and* wrong arity (`activate` vs `button_press_event`)

`plugins/taskbar/taskbar.c`'s "Move to workspace" submenu items: numbered
workspace items were wired to `button_press_event`, which never fires on a
plain (windowless) `GtkMenuItem`; the "All workspaces" item used `activate`
but with a 3-argument callback, while `activate` only dispatches
`(GtkWidget*, gpointer)` — SIGSEGV. Current (fixed) pattern:

```c
static void
send_to_workspace(GtkWidget *widget, taskbar_priv *tb)   // matches "activate"
...
g_signal_connect(G_OBJECT(mi), "activate", (GCallback)send_to_workspace, tb);
```

### BUG-018 — extra leading parameter (`size-changed`)

`plugins/xrandr/xrandr.c`'s `xrandr_update` had a spurious leading
`GtkWidget*` parameter, but `GdkScreen`'s `"size-changed"` dispatches only
`(GdkScreen*, gpointer)` — the arguments landed shifted by one slot, turning
`priv` into garbage and crashing on every monitor resize. Current (fixed)
signature:

```c
static void
xrandr_update(GdkScreen *scr, xrandr_priv *priv)
```

### BUG-019 — right signature, wrong function (`g_timeout_add`)

`plugins/timer/timer.c`'s alarm-flash timer was wired to `timer_tick`
instead of `timer_flash`. Both matched `GSourceFunc`'s signature perfectly
(so nothing caught it at compile time), but `timer_tick` self-cancels
(returns `FALSE`) immediately in the `ALARMED` state, so the "DONE" label
never flashed. A correct signature is necessary but not sufficient — the
function also has to be the *right* one.

---

## Auditing for regressions

Re-find every registration covered by this page and cross-check each
against the tables above:

```bash
grep -rn 'g_signal_connect\|g_timeout_add\|gdk_window_add_filter' --include=*.c .
```

New signal names this doesn't cover yet can be looked up in the GTK2/GDK2
API reference; add a row here once you've confirmed the canonical signature.
