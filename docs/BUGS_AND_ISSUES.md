# Bugs and Issues

This file documents bugs and code issues found during the inline-comment review
of the fbpanel source tree.  All 17 tracked bugs have been fixed.

Severity levels used below:
- **CRASH** – can cause a segfault, use-after-free, or other process-terminating fault
- **RESOURCE LEAK** – memory, file descriptor, or GObject ref is not released
- **LOGIC ERROR** – incorrect computation that produces wrong output
- **TYPE MISMATCH** – C type-system violation (undefined behaviour, silent miscast)
- **DEAD CODE** – unreachable or unused variables/branches
- **MINOR** – cosmetic or stylistic issue with no runtime impact

---

## plugins/cpu/cpu.c

### BUG-001 — Stub parameter name mismatch (compile error on unsupported platforms)

**Severity:** CRASH (compile-time error on non-Linux, non-FreeBSD targets)
**Status:** Fixed

In the `#else` (unsupported platform) branch of `cpu_get_load_real`, the stub
body references `cpu`:

```c
static int cpu_get_load_real(cpu_priv *cpu)
{
    // ...
    #else
    ERR("%s: unsupported\n", cpu->plugin.klass->type);
    //                       ^^^
    // BUG: parameter is named 'cpu', but the surrounding block uses 's'
    // on Linux (parameter name 's'), causing a compile error on
    // non-Linux, non-FreeBSD platforms.
```

The Linux and FreeBSD implementations use `s` as the parameter name; only the
stub uses `cpu`.  The code never compiled on unsupported platforms because the
function's Linux/FreeBSD bodies name the parameter `s`, while the stub body
references the non-existent `cpu` name.

**Fix applied:** Renamed the stub body to use `s` and changed to
`memset(s, 0, sizeof(struct cpu_stat)); return -1;`.

---

### BUG-002 — Uninitialized variables `a` and `b` used in debug trace

**Severity:** MINOR (only affects debug builds, but is technically UB)
**Status:** Fixed

In `cpu_update()`, the variables `a` and `b` are declared but only assigned
inside the `if (!cpu_get_load_real(c)) goto end` block.  If `cpu_get_load_real`
fails and execution jumps to `end:`, the subsequent `DBG("cpu: %lu %lu ...", a, b)`
call reads uninitialized values.

**Fix applied:** Changed `gfloat a, b;` to `gfloat a = 0.0, b = 0.0;`.

---

## plugins/launchbar/launchbar.c

### BUG-003 — Double-destroy of `lb->box` in destructor

**Severity:** CRASH (use-after-free / double-free)
**Status:** Fixed

`launchbar_destructor` called `gtk_widget_destroy(lb->box)` explicitly.
However, `lb->box` is a child of `p->pwid`; when the plugin framework
subsequently destroys `p->pwid`, GTK destroys all its children, including
`lb->box` a second time.

**Fix applied:** Removed the explicit `gtk_widget_destroy(lb->box)` call.

---

## plugins/mem/mem.c

### BUG-004 — Double-destroy of `mem->box` in destructor

**Severity:** CRASH (use-after-free / double-free)
**Status:** Fixed

Same pattern as BUG-003.  `mem_destructor` called `gtk_widget_destroy(mem->box)`
explicitly, but `mem->box` is already a child of `p->pwid` and will be destroyed
again by the framework.

**Fix applied:** Removed the explicit `gtk_widget_destroy(mem->box)` call.

---

## plugins/mem2/mem2.c

### BUG-005 — Non-Linux `mem_usage` stub has incompatible signature

**Severity:** TYPE MISMATCH / CRASH (on non-Linux platforms)
**Status:** Fixed

The Linux implementation of `mem_usage` is:

```c
static void mem_usage(mem2_priv *c)  { ... }
```

The non-Linux stub was:

```c
static int mem_usage()  { ... }   // different return type, no parameter
```

The function is scheduled via `g_timeout_add((GSourceFunc) mem_usage, c)`.
On non-Linux platforms the cast to `GSourceFunc` hides the incompatibility,
but calling it with a `mem2_priv *` argument that the stub ignores, and
interpreting the `int` return as `gboolean`, is undefined behaviour.

**Fix applied:** Changed the non-Linux stub to
`static gboolean mem_usage(mem2_priv *c) { (void)c; return TRUE; }`.

---

## plugins/pager/pager.c

### BUG-006 — `scalew` and `scaleh` are swapped in `desk_configure_event`

**Severity:** LOGIC ERROR (thumbnail scaling incorrect after resize)
**Status:** Fixed

```c
d->scalew = (double) h / (double) screen_h;   // BUG: uses h (height) for scalew (width scale)
d->scaleh = (double) w / (double) screen_w;   // BUG: uses w (width)  for scaleh (height scale)
```

**Fix applied:**
```c
d->scalew = (gfloat)w / (gfloat)gdk_screen_width();
d->scaleh = (gfloat)h / (gfloat)gdk_screen_height();
```

---

### BUG-007 — `pg->gen_pixbuf` is never unreferenced in the destructor

**Severity:** RESOURCE LEAK (GdkPixbuf reference)
**Status:** Fixed

`pg->gen_pixbuf` is allocated in `pager_constructor` via `gdk_pixbuf_new_from_xpm_data()`
and stored as the "generic" desktop thumbnail pixbuf.  It was never `g_object_unref`'d.

**Fix applied:** Added `if (pg->gen_pixbuf) g_object_unref(pg->gen_pixbuf);` in
`pager_destructor`.

---

## plugins/taskbar/taskbar.c

### BUG-008 — FbEv signal connections for `tb_make_menu` are never disconnected

**Severity:** CRASH (use-after-free when FbEv fires after plugin unload)
**Status:** Fixed

In `taskbar_constructor`, two FbEv signals were connected to `tb_make_menu`
but were not disconnected in the destructor.

**Fix applied:** Added to `taskbar_destructor`:
```c
g_signal_handlers_disconnect_by_func(G_OBJECT(fbev), tb_make_menu, tb);
```

---

### BUG-009 — `use_net_active` is a file-scope static shared across instances

**Severity:** LOGIC ERROR (incorrect behaviour with multiple taskbar instances)
**Status:** Fixed

`use_net_active` was a file-scope `static gboolean`, shared across all taskbar
plugin instances.

**Fix applied:** Moved `use_net_active` into `taskbar_priv` so each instance
has its own copy.  Updated `net_active_detect()` signature to accept
`taskbar_priv *tb` and set `tb->use_net_active` directly.

---

## plugins/tray/fixedtip.c

### BUG-010 — `expose_handler` has wrong first-parameter type

**Severity:** TYPE MISMATCH (undefined behaviour; currently harmless by accident)
**Status:** Fixed

```c
static gboolean
expose_handler (GtkTooltips *tooltips)   // BUG: wrong type
```

The function is connected to the `"expose_event"` signal, which GTK invokes
with the prototype `(GtkWidget *widget, GdkEventExpose *event, gpointer data)`.

**Fix applied:** Changed the signature to:
```c
static gboolean
expose_handler (GtkWidget *widget, GdkEventExpose *event, gpointer data)
```

---

## plugins/volume/volume.c

### BUG-011 — `/dev/mixer` file descriptor is never closed

**Severity:** RESOURCE LEAK (file descriptor leak)
**Status:** Fixed

`volume_constructor` opens `/dev/mixer` but `volume_destructor` did not call
`close(c->fd)`.

**Fix applied:** Added `close(c->fd);` in `volume_destructor` before
`class_put("meter")`.

---

## plugins/wincmd/wincmd.c

### BUG-012 — Button1/Button2 config values are parsed but never used

**Severity:** MINOR (user configuration has no effect)
**Status:** Fixed

`wincmd_constructor` reads `Button1` and `Button2` xconf keys into
`wc->button1` and `wc->button2`, but the `clicked()` handler previously ignored
those fields and hardcoded the actions.

**Fix applied:** Added `do_action()` helper and updated `clicked()` to dispatch
based on `wc->button1` and `wc->button2`.

---

### BUG-013 — `action1` field is declared but never used

**Severity:** DEAD CODE
**Status:** Fixed (superseded by BUG-012 fix)

`action1` was unused because `clicked()` hardcoded the per-button actions.
After the BUG-012 fix, `action1` is now used for the shade-direction toggle on
button 1, consistent with `action2` for button 2.

---

### BUG-014 — `pix` and `mask` fields are never populated; destructor checks are dead code

**Severity:** DEAD CODE
**Status:** Fixed

`wincmd_priv` declared `GdkPixmap *pix` and `GdkBitmap *mask` which were never
assigned.  The destructor conditionally unreferenced them, but both were always
`NULL`.

**Fix applied:** Removed the `pix` and `mask` fields from `wincmd_priv` and
simplified `wincmd_destructor` to a no-op body.

---

## plugins/volume/volume.c

### BUG-016 — `meter_destructor` not called when `volume_constructor` fails after `meter_constructor`

**Severity:** CRASH (use-after-free / SIGSEGV)
**Status:** Fixed

`volume_constructor` calls `PLUGIN_CLASS(k)->constructor(p)` (i.e. `meter_constructor`)
which creates the GtkImage widget and connects `update_view` to the global
`icon_theme "changed"` signal with `m` (a pointer to the `meter_priv` embedded
in the `volume_priv` allocation) as the swapped user-data argument.

If `open("/dev/mixer")` subsequently fails, `volume_constructor` returned 0 without
calling `meter_destructor()`.  The framework then:

1. Calls `gtk_widget_destroy(pwid)` — destroying `m->meter` (the GtkImage).
2. Calls `plugin_put(plug)` → `g_free(plug)` — **freeing the entire struct**.

The `icon_theme "changed"` signal handler remains connected, pointing to the
now-freed struct.  When the GTK icon-theme subsystem later emits "changed" (e.g.
during the first icon lookup at startup), `update_view(m)` is invoked with a
dangling pointer.  Reading garbage from the freed memory produces the observed
crash sequence:

```
meter: illegal level -1
(fbpanel): Gtk-CRITICAL: IA__gtk_icon_theme_load_icon: assertion 'icon_name != NULL' failed
Segmentation fault (core dumped)
```

The SIGSEGV at `si_addr=0x20` is consistent with `g_type_check_instance_cast`
dereferencing a stale `m->meter` pointer (the GLib type-check reads
`instance->g_class` and then `g_class->g_type`; a freed / zeroed pointer at
that location gives address 0x20).

**Fix applied:** In the `/dev/mixer` failure path, call
`PLUGIN_CLASS(k)->destructor(p)` (which disconnects the signal) and
`class_put("meter")` before returning 0.

---

## plugins/meter/meter.c

### BUG-015 — `update_view` always short-circuits; icons never reload on theme change

**Severity:** LOGIC ERROR (icon not refreshed when GTK icon theme changes)
**Status:** Fixed

`update_view` is supposed to force a reload of the current icon after a GTK
icon-theme change:

```c
static void update_view(meter_priv *m) {
    m->cur_icon = -1;                    /* invalidate cached index */
    meter_set_level(m, m->level);        /* should reload icon      */
}
```

However, `meter_set_level` contains:

```c
if (m->level == level)   /* gfloat == int */
    RET();
```

`m->level` is a `gfloat` always holding an exact-integer value (0, -1, or a
level 0..100 previously stored via `m->level = level`).  When `update_view`
passes `m->level` as the `int level` argument, the C implicit float→int
conversion produces the same integer value, and the subsequent int→float
promotion for the comparison makes `m->level == level` always `TRUE`.  The
function returns immediately, never reaching the icon-load code.

**Fix applied:** Added `&& m->cur_icon != -1` to the early-exit condition:

```c
if (m->level == level && m->cur_icon != -1)
    RET();
```

When `update_view` has reset `m->cur_icon` to −1, the guard is `FALSE` and
`meter_set_level` proceeds to reload the icon from the new theme.

---

### BUG-017 — All "Move to workspace" items broken; "All workspaces" crashes

**File:** `plugins/taskbar/taskbar.c` — `send_to_workspace()`, `tb_make_menu()`
**Severity:** CRASH ("All workspaces") + LOGIC ERROR (numbered items)
**Status:** Fixed

**Symptom observed:** Right-clicking a taskbar button → "Move to workspace →
All workspaces" → immediate SIGSEGV.

**Symptom on numbered items:** Clicking "Move to workspace → Workspace N"
does nothing — the window is not moved.

**Root cause — two related problems:**

**Problem 1 (numbered items): `button_press_event` never fires on GtkMenuItem.**
GtkMenuItems inside a popup menu do not have their own `GdkWindow`; they are
"no-window" widgets.  The GtkMenu grabs the pointer and handles all button
events at the menu-window level internally, dispatching only `activate` to the
selected item on button-release.  User handlers connected to
`button_press_event` on a GtkMenuItem never fire.  Every numbered workspace
item was silently broken — the `Xclimsg` call to move the window was never
reached.

Additionally, `button_press_event` expects a `gboolean` return; the callback
returns `void`.  After `Xclimsg` returns (typically 1), the undefined rax=1
would be read by GTK as TRUE (event consumed), potentially preventing the menu
from closing.

**Problem 2 ("All workspaces"): wrong callback arity for `activate`.**
An attempt to use `"activate"` for the "All workspaces" item used the
3-argument `send_to_workspace` callback.  The `activate` signal only provides
`(widget, user_data)`.  With x86-64 SysV ABI:

- `rdi` → widget
- `rsi` → `tb`  (user_data, lands in the `void *iii` slot)
- `rdx` → **garbage** (never set by GTK, read as the `taskbar_priv *tb` arg)

`tb->menutask->win` then dereferences the garbage `rdx` register → SIGSEGV.

Note: the developer's comment `/* NOTE: uses button_press_event not activate
— send_to_workspace gets 3 args */` shows awareness of the arity mismatch
but not of the fact that `button_press_event` itself does not fire on
windowless GtkMenuItems.

**Fix applied:** Rewrote `send_to_workspace` as a proper 2-argument `activate`
handler (matching `menu_raise_window` and `menu_iconify_window`), and switched
all workspace submenu items — numbered and "All workspaces" — to `"activate"`:

```c
/* Before (broken): 3-arg callback, button_press_event on windowless widget */
static void send_to_workspace(GtkWidget *widget, void *iii, taskbar_priv *tb)
g_signal_connect(G_OBJECT(mi), "button_press_event",
    (GCallback)send_to_workspace, tb);

/* After (correct): 2-arg activate handler, consistent with Raise/Iconify/Close */
static void send_to_workspace(GtkWidget *widget, taskbar_priv *tb)
g_signal_connect(G_OBJECT(mi), "activate",
    (GCallback)send_to_workspace, tb);
```

---

## Summary Table

| ID       | File                          | Severity        | Status  | Description                                           |
|----------|-------------------------------|-----------------|---------|-------------------------------------------------------|
| BUG-001  | plugins/cpu/cpu.c             | CRASH (compile) | Fixed   | Stub uses wrong parameter name `cpu` vs `s`           |
| BUG-002  | plugins/cpu/cpu.c             | MINOR           | Fixed   | Uninitialized `a`, `b` read in debug trace after goto |
| BUG-003  | plugins/launchbar/launchbar.c | CRASH           | Fixed   | Double-destroy of `lb->box` in destructor             |
| BUG-004  | plugins/mem/mem.c             | CRASH           | Fixed   | Double-destroy of `mem->box` in destructor            |
| BUG-005  | plugins/mem2/mem2.c           | TYPE MISMATCH   | Fixed   | Non-Linux `mem_usage` stub has incompatible signature |
| BUG-006  | plugins/pager/pager.c         | LOGIC ERROR     | Fixed   | `scalew`/`scaleh` swapped in `desk_configure_event`   |
| BUG-007  | plugins/pager/pager.c         | RESOURCE LEAK   | Fixed   | `pg->gen_pixbuf` never `g_object_unref`'d             |
| BUG-008  | plugins/taskbar/taskbar.c     | CRASH           | Fixed   | `tb_make_menu` FbEv signals never disconnected        |
| BUG-009  | plugins/taskbar/taskbar.c     | LOGIC ERROR     | Fixed   | `use_net_active` static shared across instances       |
| BUG-010  | plugins/tray/fixedtip.c       | TYPE MISMATCH   | Fixed   | `expose_handler` first parameter type is wrong        |
| BUG-011  | plugins/volume/volume.c       | RESOURCE LEAK   | Fixed   | `/dev/mixer` fd never closed in destructor            |
| BUG-012  | plugins/wincmd/wincmd.c       | MINOR           | Fixed   | Button1/Button2 config values parsed but ignored      |
| BUG-013  | plugins/wincmd/wincmd.c       | DEAD CODE       | Fixed   | `action1` field declared but never used               |
| BUG-014  | plugins/wincmd/wincmd.c       | DEAD CODE       | Fixed   | `pix`/`mask` fields never populated; dead destructor  |
| BUG-015  | plugins/meter/meter.c         | LOGIC ERROR     | Fixed   | `update_view` always short-circuits; icons never reload on theme change |
| BUG-016  | plugins/volume/volume.c       | CRASH           | Fixed   | `meter_destructor` not called on `/dev/mixer` failure → use-after-free |
| BUG-017  | plugins/taskbar/taskbar.c     | CRASH + LOGIC   | Fixed   | All "Move to workspace" items broken: numbered items use button_press_event (never fires on windowless GtkMenuItems); "All workspaces" uses activate with 3-arg callback → SIGSEGV |
