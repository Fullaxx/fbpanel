# Bugs and Issues

This file documents bugs and code issues found during the inline-comment review
of the fbpanel source tree.  All 14 tracked bugs have been fixed.

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
