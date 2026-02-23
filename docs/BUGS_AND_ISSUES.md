# Bugs and Issues

This file documents bugs and code issues found during the inline-comment review
of the fbpanel source tree.  No fixes are applied here; this is a tracking
document only.

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
references `cpu`.  The actual function signature uses `s` in the `#ifdef` chain
but the `#else` stub refers to the non-existent `cpu` name.

**Fix:** Rename the stub parameter to `s` to match the other branches, or
rename the `cpu` reference to `s`.

---

### BUG-002 — Uninitialized variables `a` and `b` used in debug trace

**Severity:** MINOR (only affects debug builds, but is technically UB)

In `cpu_update()`, the variables `a` and `b` are declared but only assigned
inside the `if (!cpu_get_load_real(c)) goto end` block.  If `cpu_get_load_real`
fails and execution jumps to `end:`, the subsequent `DBG("cpu: %lu %lu ...", a, b)`
call reads uninitialized values.

**Fix:** Initialize `a = 0; b = 0;` at the declaration site, or restructure
to avoid using them after the `goto`.

---

## plugins/launchbar/launchbar.c

### BUG-003 — Double-destroy of `lb->box` in destructor

**Severity:** CRASH (use-after-free / double-free)

`launchbar_destructor` calls `gtk_widget_destroy(lb->box)` explicitly.
However, `lb->box` is a child of `p->pwid`; when the plugin framework
subsequently destroys `p->pwid`, GTK will destroy all its children, including
`lb->box` a second time.  Calling `gtk_widget_destroy` on an already-destroyed
widget is undefined behaviour and typically causes a segfault.

```c
static void
launchbar_destructor(plugin_instance *p)
{
    launchbar_priv *lb = (launchbar_priv *)p;
    ENTER;
    gtk_widget_destroy(lb->box);   // BUG: double-destroy
    RET();
}
```

**Fix:** Remove the explicit `gtk_widget_destroy(lb->box)` call.  The parent
framework destroys `p->pwid` and all its children automatically.

---

## plugins/mem/mem.c

### BUG-004 — Double-destroy of `mem->box` in destructor

**Severity:** CRASH (use-after-free / double-free)

Same pattern as BUG-003.  `mem_destructor` calls `gtk_widget_destroy(mem->box)`
explicitly, but `mem->box` is already a child of `p->pwid` and will be destroyed
again by the framework.

```c
static void
mem_destructor(plugin_instance *p)
{
    mem_priv *mem = (mem_priv *)p;
    gtk_widget_destroy(mem->box);  // BUG: double-destroy
}
```

**Fix:** Remove the explicit `gtk_widget_destroy(mem->box)` call.

---

## plugins/mem2/mem2.c

### BUG-005 — Non-Linux `mem_usage` stub has incompatible signature

**Severity:** TYPE MISMATCH / CRASH (on non-Linux platforms)

The Linux implementation of `mem_usage` is:

```c
static void mem_usage(mem2_priv *c)  { ... }
```

The non-Linux stub is:

```c
static int mem_usage()  { ... }   // different return type, no parameter
```

The function is scheduled via `g_timeout_add((GSourceFunc) mem_usage, c)`.
On non-Linux platforms the cast to `GSourceFunc` hides the incompatibility,
but calling it with a `mem2_priv *` argument that the stub ignores, and
interpreting the `int` return as `gboolean`, is undefined behaviour.

**Fix:** Make all platform branches share the same signature:
`static gboolean mem_usage(mem2_priv *c)`.

---

## plugins/pager/pager.c

### BUG-006 — `scalew` and `scaleh` are swapped in `desk_configure_event`

**Severity:** LOGIC ERROR (thumbnail scaling incorrect after resize)

```c
static gboolean
desk_configure_event(GtkWidget *widget, GdkEventConfigure *event, desk *d)
{
    ...
    d->scalew = (double) h / (double) screen_h;   // BUG: uses h (height) for scalew (width scale)
    d->scaleh = (double) w / (double) screen_w;   // BUG: uses w (width)  for scaleh (height scale)
```

`scalew` should be `widget_width / screen_width` and `scaleh` should be
`widget_height / screen_height`.  The swapped assignment causes the thumbnail
miniatures to stretch in the wrong direction when the pager is resized.

**Fix:**
```c
d->scalew = (double) w / (double) screen_w;
d->scaleh = (double) h / (double) screen_h;
```

---

### BUG-007 — `pg->gen_pixbuf` is never unreferenced in the destructor

**Severity:** RESOURCE LEAK (GdkPixbuf reference)

`pg->gen_pixbuf` is allocated in `pager_update_img` via `gdk_pixbuf_new()`
(or similar) and stored as the "generic" desktop thumbnail pixbuf.  The
`pager_destructor` frees individual desk pixmaps and disconnects signals, but
`pg->gen_pixbuf` is never `g_object_unref`'d, leaking the pixbuf until the
process exits.

**Fix:** Add `if (pg->gen_pixbuf) g_object_unref(pg->gen_pixbuf);` in
`pager_destructor`.

---

## plugins/taskbar/taskbar.c

### BUG-008 — FbEv signal connections for `tb_make_menu` are never disconnected

**Severity:** CRASH (use-after-free when FbEv fires after plugin unload)

In `taskbar_constructor`, two FbEv signals are connected:

```c
g_signal_connect(G_OBJECT(fbev), "number_of_desktops",
    G_CALLBACK(tb_make_menu), (gpointer)tb);
g_signal_connect(G_OBJECT(fbev), "desktop_names",
    G_CALLBACK(tb_make_menu), (gpointer)tb);
```

`taskbar_destructor` disconnects `tb_net_number_of_desktops` but does NOT
disconnect these two `tb_make_menu` connections.  If the number-of-desktops or
desktop-names root property changes after the taskbar plugin is unloaded, FbEv
will call `tb_make_menu` with a dangling `tb` pointer, causing a use-after-free.

**Fix:** In `taskbar_destructor`, add:
```c
g_signal_handlers_disconnect_by_func(G_OBJECT(fbev),
    G_CALLBACK(tb_make_menu), tb);
```

---

### BUG-009 — `use_net_active` is a file-scope static shared across instances

**Severity:** LOGIC ERROR (incorrect behaviour with multiple taskbar instances)

```c
static gboolean use_net_active;
```

This variable is set in `taskbar_constructor` based on whether the window
manager supports `_NET_ACTIVE_WINDOW`.  Because it is file-scope static, all
taskbar plugin instances share the same variable.  If two taskbar plugins are
loaded simultaneously (e.g., one per monitor in a multi-head setup), the second
instance's constructor will overwrite the value determined by the first, and the
first instance will silently use the wrong value.

**Fix:** Move `use_net_active` into `taskbar_priv` so each instance has its own.

---

## plugins/tray/fixedtip.c

### BUG-010 — `expose_handler` has wrong first-parameter type

**Severity:** TYPE MISMATCH (undefined behaviour; currently harmless by accident)

```c
static gboolean
expose_handler (GtkTooltips *tooltips)   // BUG: wrong type
{
    gtk_paint_flat_box (tip->style, tip->window, ...);
    return FALSE;
}
```

The function is connected to the `"expose_event"` signal, which GTK invokes
with the prototype `(GtkWidget *widget, GdkEventExpose *event, gpointer data)`.
The declared first parameter `GtkTooltips *tooltips` does not match.

The body happens to work correctly in practice because it uses the file-scope
static `tip` rather than the callback argument, but passing a `GtkWidget *`
where a `GtkTooltips *` is declared is undefined behaviour under the C standard.

**Fix:** Change the signature to:
```c
static gboolean
expose_handler (GtkWidget *widget, GdkEventExpose *event, gpointer data)
```

---

## plugins/volume/volume.c

### BUG-011 — `/dev/mixer` file descriptor is never closed

**Severity:** RESOURCE LEAK (file descriptor leak)

`volume_constructor` opens `/dev/mixer`:

```c
if ((c->fd = open("/dev/mixer", O_RDWR, 0)) < 0) { ... }
```

`volume_destructor` does not call `close(c->fd)`:

```c
static void
volume_destructor(plugin_instance *p)
{
    volume_priv *c = (volume_priv *) p;
    g_source_remove(c->update_id);
    if (c->slider_window)
        gtk_widget_destroy(c->slider_window);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("meter");
    // close(c->fd) is MISSING
}
```

Each time the volume plugin is loaded and unloaded, the process leaks one file
descriptor.

**Fix:** Add `close(c->fd);` in `volume_destructor` before `class_put("meter")`.

---

## plugins/wincmd/wincmd.c

### BUG-012 — Button1/Button2 config values are parsed but never used

**Severity:** MINOR (user configuration has no effect)

`wincmd_constructor` reads `Button1` and `Button2` xconf keys into
`wc->button1` and `wc->button2`, but the `clicked()` handler ignores those
fields and hardcodes:

- Left-click (button 1) → `toggle_iconify`
- Middle-click (button 2) → toggle `action2` + `toggle_shaded`

Regardless of what the user configures for `Button1`/`Button2`, the behaviour
is always the same.

**Fix:** In `clicked()`, dispatch on `wc->button1` and `wc->button2` instead of
hardcoding the action per button number.

---

### BUG-013 — `action1` field is declared but never used

**Severity:** DEAD CODE

`wincmd_priv.action1` is declared alongside `action2`, but only `action2` is
ever read or written.  `action1` is always zero and serves no purpose.

**Fix:** Remove the `action1` field from `wincmd_priv`.

---

### BUG-014 — `pix` and `mask` fields are never populated; destructor checks are dead code

**Severity:** DEAD CODE

`wincmd_priv` declares:

```c
GdkPixmap *pix;
GdkBitmap *mask;
```

Neither field is ever assigned in `wincmd_constructor` (which uses
`fb_button_new` for the widget).  `wincmd_destructor` conditionally unrefs
them, but both are always `NULL`, making those branches unreachable.

**Fix:** Remove the `pix` and `mask` fields from `wincmd_priv` and remove the
corresponding destructor code.

---

## Summary Table

| ID       | File                          | Severity        | Description                                           |
|----------|-------------------------------|-----------------|-------------------------------------------------------|
| BUG-001  | plugins/cpu/cpu.c             | CRASH (compile) | Stub uses wrong parameter name `cpu` vs `s`           |
| BUG-002  | plugins/cpu/cpu.c             | MINOR           | Uninitialized `a`, `b` read in debug trace after goto |
| BUG-003  | plugins/launchbar/launchbar.c | CRASH           | Double-destroy of `lb->box` in destructor             |
| BUG-004  | plugins/mem/mem.c             | CRASH           | Double-destroy of `mem->box` in destructor            |
| BUG-005  | plugins/mem2/mem2.c           | TYPE MISMATCH   | Non-Linux `mem_usage` stub has incompatible signature |
| BUG-006  | plugins/pager/pager.c         | LOGIC ERROR     | `scalew`/`scaleh` swapped in `desk_configure_event`   |
| BUG-007  | plugins/pager/pager.c         | RESOURCE LEAK   | `pg->gen_pixbuf` never `g_object_unref`'d             |
| BUG-008  | plugins/taskbar/taskbar.c     | CRASH           | `tb_make_menu` FbEv signals never disconnected        |
| BUG-009  | plugins/taskbar/taskbar.c     | LOGIC ERROR     | `use_net_active` static shared across instances       |
| BUG-010  | plugins/tray/fixedtip.c       | TYPE MISMATCH   | `expose_handler` first parameter type is wrong        |
| BUG-011  | plugins/volume/volume.c       | RESOURCE LEAK   | `/dev/mixer` fd never closed in destructor            |
| BUG-012  | plugins/wincmd/wincmd.c       | MINOR           | Button1/Button2 config values parsed but ignored      |
| BUG-013  | plugins/wincmd/wincmd.c       | DEAD CODE       | `action1` field declared but never used               |
| BUG-014  | plugins/wincmd/wincmd.c       | DEAD CODE       | `pix`/`mask` fields never populated; dead destructor  |
