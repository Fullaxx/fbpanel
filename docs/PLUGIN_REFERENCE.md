# fbpanel — Plugin API Reference

This document is the definitive reference for writing fbpanel plugins.

---

## Overview

Every fbpanel plugin is a shared library (`.so`) that is dynamically
loaded by the panel at startup. A plugin registers itself via the
`plugin_class` descriptor and is instantiated once per `Plugin { }`
block in the user's config file.

---

## Plugin file structure

A minimal single-file plugin:

```c
#include "plugin.h"

/* Private data for one instance of this plugin */
typedef struct {
    plugin_instance plugin;   /* MUST be the first member */
    GtkWidget      *label;
    guint           timer_id;
    gchar          *format;
} my_priv;

/* Periodic callback — update the label */
static gboolean
my_update(my_priv *priv)
{
    char buf[64];
    /* ... fill buf ... */
    gtk_label_set_text(GTK_LABEL(priv->label), buf);
    return TRUE;   /* keep firing */
}

/* Constructor — called once per Plugin block in the config */
static int
my_constructor(plugin_instance *p)
{
    my_priv *priv = (my_priv *) p;

    /* Read config values */
    XCG(p->xc, "format", &priv->format, str);

    /* Build the widget tree inside p->pwid */
    priv->label = gtk_label_new(NULL);
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);

    /* Start a periodic timer */
    priv->timer_id = g_timeout_add(1000, (GSourceFunc) my_update, priv);
    my_update(priv);   /* populate immediately */

    return 1;   /* 1 = success, 0 = failure (plugin skipped) */
}

/* Destructor — paired with constructor; called before pwid is destroyed */
static void
my_destructor(plugin_instance *p)
{
    my_priv *priv = (my_priv *) p;

    /* 1. Remove timers */
    if (priv->timer_id) {
        g_source_remove(priv->timer_id);
        priv->timer_id = 0;
    }

    /* 2. Disconnect signals on objects that outlive p->pwid */
    /* (signals on child widgets are cleaned up automatically) */

    /* 3. Free owned memory */
    /* priv->format is owned by xconf — do not free */
}

/* Plugin class descriptor — one per plugin type */
static plugin_class class = {
    .type        = "myplugin",
    .name        = "My Plugin",
    .version     = "1.0",
    .description = "Example plugin",
    .priv_size   = sizeof(my_priv),
    .constructor = my_constructor,
    .destructor  = my_destructor,
};

/* Required: pointer used by the PLUGIN macro for auto-registration */
static plugin_class *class_ptr = (plugin_class *) &class;
```

---

## `plugin_class` fields

```c
typedef struct {
    gchar         *type;         /* Unique type string, matches config "type" */
    gchar         *name;         /* Human-readable name (for preferences dialog) */
    gchar         *version;      /* Version string */
    gchar         *description;  /* Short description */
    int            priv_size;    /* sizeof(my_priv) — allocated by panel */
    int          (*constructor)(plugin_instance *);  /* Returns 1=ok, 0=fail */
    void         (*destructor) (plugin_instance *);  /* Cleanup */
    /* Optional fields: */
    void         (*config)     (plugin_instance *, GtkWidget *); /* Config UI */
    void         (*save)       (plugin_instance *, FILE *);      /* Save config */
    gboolean       expand;       /* TRUE: plugin expands to fill available space */
    gboolean       setorientation;  /* TRUE: called when panel orientation changes */
} plugin_class;
```

---

## `plugin_instance` fields

```c
typedef struct {
    plugin_class  *class;   /* Pointer to the plugin's class descriptor */
    panel         *panel;   /* Pointer to the panel struct */
    xconf         *xc;      /* Plugin's config subtree (NOT owned by plugin) */
    GtkWidget     *pwid;    /* The plugin's container widget (GtkBgbox) */
} plugin_instance;
```

**Critical:** `plugin_instance` **must be the first member** of the
plugin's private struct. This allows safe casting between
`plugin_instance *` and `my_priv *`.

---

## The PLUGIN macro

When a plugin `.c` file is compiled with `-DPLUGIN` (set by CMakeLists.txt
for all plugin targets), including `plugin.h` defines:

```c
/* Auto-register class_ptr when .so is dlopen'd */
static void ctor(void) __attribute__((constructor));
static void ctor(void) { class_register(class_ptr); }

/* Auto-unregister when .so is dlclose'd */
static void dtor(void) __attribute__((destructor));
static void dtor(void) { class_unregister(class_ptr); }
```

The plugin must define `static plugin_class *class_ptr` at file scope,
pointing to its `plugin_class` descriptor.

### Multi-file plugins

For plugins split across multiple `.c` files (e.g., taskbar), the
shared internal header must `#undef PLUGIN` before `#include "plugin.h"`
to avoid duplicate `ctor`/`dtor` symbols:

```c
/* taskbar_priv.h */
#undef PLUGIN
#include "plugin.h"
```

Then exactly one `.c` file defines `class_ptr` and the registration
macro fires normally.

---

## Reading config values — `XCG()` macro

The `XCG()` macro reads typed values from the plugin's config subtree
(`p->xc`):

```c
XCG(p->xc, "key", &variable, type);
```

| Type token | C type | Example |
|-----------|--------|---------|
| `int` | `int` | `XCG(p->xc, "width", &priv->width, int);` |
| `str` | `gchar *` | `XCG(p->xc, "label", &priv->text, str);` |
| `enum` | `int` | `XCG(p->xc, "align", &priv->align, enum, align_enum);` |

For `enum`, the last argument is a `xconf_enum[]` table mapping strings
to integers:

```c
static xconf_enum bool_enum[] = {
    { "true",  1 },
    { "yes",   1 },
    { "false", 0 },
    { "no",    0 },
    { NULL,    0 },
};
XCG(p->xc, "icons_only", &priv->icons_only, enum, bool_enum);
```

**String values** (`str`) are owned by the xconf tree. Do **not** call
`g_free()` on them unless you first `g_strdup()` them.

---

## Helper plugins

Some plugins are reusable widget libraries:

### `chart` — scrolling bar graph

```c
#include "chart.h"

/* In constructor: */
priv->chart = chart_new();
gtk_container_add(GTK_CONTAINER(p->pwid), priv->chart->widget);
gtk_widget_show(priv->chart->widget);
chart_set_range(priv->chart, 0, 100);

/* In update callback: */
chart_push_value(priv->chart, current_value);
```

### `meter` — icon-based level indicator

```c
/* In constructor: */
priv->meter_class = class_get("meter");
if (!priv->meter_class) return 0;
priv->meter = plugin_load(priv->meter_class, p->panel, xc);

/* In update callback: */
meter_set_level(priv->meter, level_0_to_100);

/* In destructor: */
plugin_put(priv->meter);
class_put("meter");
```

---

## The panel event bus — `FbEv`

EWMH property changes on the root window are broadcast as GObject
signals on the global `fbev` object (declared in `panel.h`).
Connect to these in the constructor; disconnect in the destructor.

| Signal | Fired when |
|--------|-----------|
| `current_desktop` | `_NET_CURRENT_DESKTOP` changes |
| `active_window` | `_NET_ACTIVE_WINDOW` changes |
| `number_of_desktops` | `_NET_NUMBER_OF_DESKTOPS` changes |
| `client_list` | `_NET_CLIENT_LIST` changes |
| `desktop_names` | `_NET_DESKTOP_NAMES` changes |

```c
/* constructor */
g_signal_connect(G_OBJECT(fbev), "current_desktop",
    G_CALLBACK(my_desktop_changed), priv);

/* destructor */
g_signal_handlers_disconnect_by_func(G_OBJECT(fbev),
    G_CALLBACK(my_desktop_changed), priv);
```

---

## Pseudo-transparent backgrounds

Each plugin's `pwid` is a `GtkBgbox`. When the panel is configured with
`transparent = true`, the panel calls:

```c
gtk_bgbox_set_background(p->pwid, BG_ROOT, tintcolor, alpha);
```

This causes `pwid` to sample the correct portion of the root window
pixmap as its background, creating the pseudo-transparency effect.
Plugins do not need to do anything special — it is handled automatically.

---

## Adding a new plugin — checklist

1. Create `plugins/<type>/` directory.
2. Write `<type>.c`:
   - `typedef struct { plugin_instance plugin; ... } my_priv;`
   - `static int my_constructor(plugin_instance *p)` returning 1 or 0
   - `static void my_destructor(plugin_instance *p)` cleaning up all resources
   - `static plugin_class class = { .type = "<type>", ... };`
   - `static plugin_class *class_ptr = (plugin_class *) &class;`
3. Add `<type>` to the `set(PLUGINS ...)` list in `CMakeLists.txt`.
4. Add a `Plugin { type = <type> }` block to `data/config/default.in`
   if the plugin should be part of the default layout.

---

## Plugin requirements

| # | Requirement |
|---|-------------|
| R1 | Constructor must return `1` on success and `0` on failure. |
| R2 | **Never call `exit()`, `abort()`, or otherwise terminate the process.** |
| R3 | Return `0` with a `g_message()` diagnostic for missing hardware/resources. |
| R4 | The panel treats constructor `0` as non-fatal: plugin is skipped. |
| R5 | Destructor must cancel all timers, disconnect all signals, close all FDs. |
| R6 | Destructor must **not** destroy `p->pwid` — that is the panel's job. |
| R7 | `plugin_instance` must be the **first member** of the private struct. |
| R8 | Multi-file plugins must `#undef PLUGIN` in their shared internal header. |
| R9 | Store the return value of every `g_timeout_add()` to allow removal. |
| R10 | Disconnect from `fbev` signals in the destructor. |
