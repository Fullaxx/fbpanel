
#ifndef PLUGIN_H
#define PLUGIN_H

/**
 * @file
 * @brief Public interface for the fbpanel plugin subsystem.
 *
 * fbpanel supports dynamically loaded plugins (shared libraries) as well as
 * plugins compiled directly into the panel binary ("static" plugins).
 *
 * Each plugin type is described by a ::plugin_class struct, which acts as a
 * vtable plus metadata. At most one plugin_class exists per plugin type
 * name in the global class registry (`class_ht`, a GHashTable in plugin.c).
 *
 * Each loaded instance of a plugin is described by a ::plugin_instance
 * struct. Multiple instances of the same type may coexist; they all share
 * a pointer to the same plugin_class.
 *
 * @par Registration lifecycle
 * Static plugins:
 *   - Registered via the `PLUGIN` macro at library-load time
 *     (`__attribute__((constructor))`).
 *   - Unregistered at library-unload time (`__attribute__((destructor))`).
 *
 * Dynamic plugins (shared libraries):
 *   - Loaded on demand by class_get() using GModule (dlopen wrapper).
 *   - The shared library's constructor function calls class_register().
 *   - class_put() decrements a reference count and, when it reaches zero,
 *     opens the module a second time then closes it twice to undo the
 *     initial open (a workaround for GModule's lack of a "close once"
 *     without a matching open).
 *
 * @par Memory ownership
 *   - plugin_class structs are allocated and owned by the plugin itself
 *     (typically a static variable inside the plugin's .so).
 *   - plugin_instance structs are allocated by plugin_load() with
 *     g_malloc0() and freed by plugin_put() with g_free(). The embedded
 *     plugin_class* pointer is non-owning (do not free it via the instance).
 *   - The GtkWidget* pwid inside plugin_instance is added to the panel's
 *     GtkBox container in plugin_start(); the container owns the widget
 *     reference. plugin_stop() calls gtk_widget_destroy(pwid) to remove it.
 */

#include <gmodule.h>  // GModule for dynamic library loading


#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <stdio.h>
#include "panel.h"   // panel struct (the main panel state)

/* Forward declaration; defined below. */
struct _plugin_instance;

/**
 * @brief Descriptor / vtable for a plugin type.
 *
 * One of these exists per plugin type name. For dynamic plugins, the
 * fields marked "pointer to data within loaded dll" point directly into
 * the loaded shared library's read-only data segment; they become invalid
 * if the library is unloaded.
 */
typedef struct {
    /* common */
    char *fname;       /**< Reserved; not used in the current codebase. File name of the shared library, or NULL for static plugins. Not populated or freed by plugin.c. */
    int count;         /**< Reference count of active plugin_instance objects of this type. Incremented by class_get(), decremented by class_put(); when it reaches zero for a dynamic plugin, the .so is closed. */
    GModule *gmodule;  /**< Handle to the GModule (shared library) for dynamic plugins; NULL for static (built-in) plugins.
                         *   @note Declared but never populated by plugin.c: class_get() does not store the GModule handle, so the .so cannot be individually closed via this pointer -- it relies on the double-open/double-close trick in class_put() instead. */

    int dynamic : 1;   /**< 1 if loaded dynamically after panel startup; 0 if a static (built-in) plugin registered before the_panel was initialised. */
    int invisible : 1; /**< 1 if the plugin has no visible widget; such plugins get a hidden GtkVBox placeholder so they occupy a slot in the panel's box (preserving child ordering). */

    /* these fields are pointers to the data within loaded dll */
    char *type;        /**< Short ASCII identifier string, e.g. "taskbar", "clock". Used as the key in the class registry hash table. Points into the plugin's own data; NOT g_free'd by plugin.c. */
    char *name;        /**< Human-readable display name, e.g. "Task Bar". Points into the plugin's own data; NOT g_free'd by plugin.c. */
    char *version;     /**< Version string of the plugin. */
    char *description; /**< One-line description of the plugin. */
    int priv_size;     /**< Size in bytes of the plugin_instance subclass struct; plugin_load() allocates this many bytes with g_malloc0(). Must be >= sizeof(plugin_instance). */

    /* virtual function table */
    /** Called by plugin_start() after the pwid widget has been created and
     *  added to the panel. Should complete plugin initialisation.
     *  @return Non-zero on success, 0 on failure. */
    int (*constructor)(struct _plugin_instance *this);
    /** Called by plugin_stop() before pwid is destroyed. Must release all
     *  plugin-owned resources (timers, signals, etc.). Must NOT call
     *  gtk_widget_destroy(this->pwid); plugin_stop() does that after
     *  destructor returns. */
    void (*destructor)(struct _plugin_instance *this);
    /** Serialises the plugin's configuration to fp (an open FILE*). May be
     *  NULL if the plugin has no persistent configuration. */
    void (*save_config)(struct _plugin_instance *this, FILE *fp);
    /** Returns a newly-created GtkWidget* tree for the plugin's
     *  configuration UI (to be embedded in a preferences dialog). Caller
     *  owns the returned widget. May be NULL; default_plugin_edit_config()
     *  is used as a fallback in that case. */
    GtkWidget *(*edit_config)(struct _plugin_instance *this);
} plugin_class;

/**
 * @def PLUGIN_CLASS(class)
 * @brief Convenience cast from a generic/void pointer to plugin_class*.
 *
 * Used in plugin code to avoid explicit casts.
 */
#define PLUGIN_CLASS(class) ((plugin_class *) class)

/**
 * @brief Per-instance state for a running plugin.
 *
 * Allocated by plugin_load() as a zero-filled block of `pc->priv_size`
 * bytes. The first `sizeof(plugin_instance)` bytes are this struct;
 * additional bytes are the plugin-specific "private" data (a poor-man's C
 * inheritance trick). Plugins cast `(plugin_instance*)` to their own larger
 * struct type to access their private fields.
 *
 * Freed by plugin_put() via g_free().
 */
typedef struct _plugin_instance{
    plugin_class *class;   /**< Non-owning pointer to the shared plugin_class descriptor. Valid for the lifetime of the plugin_instance. */
    panel        *panel;   /**< Non-owning pointer to the panel that hosts this plugin. */
    xconf        *xc;      /**< Parsed configuration node for this plugin instance (from xconf). Ownership follows xconf conventions; not managed by plugin.c. */
    GtkWidget    *pwid;    /**< Container widget for this plugin's UI: a GtkBgbox for visible plugins, or a hidden GtkVBox placeholder for invisible ones. The panel's GtkBox container holds a reference to it; plugin_stop() calls gtk_widget_destroy(pwid) to release it. Plugins may add children to pwid but must not destroy it directly. */
    int           expand;  /**< Whether pwid expands to fill extra panel space (the `expand` argument to gtk_box_pack_start). */
    int           padding; /**< Extra pixels of space between this plugin and its neighbours (the `padding` argument to gtk_box_pack_start). */
    int           border;  /**< Inner border width in pixels set on pwid (via gtk_container_set_border_width). */
} plugin_instance;

/* -------------------------------------------------------------------------
 * Plugin class registry API
 * ------------------------------------------------------------------------- */

/**
 * @brief Decrement the reference count for a registered plugin class.
 *
 * If the count drops to zero and the plugin is dynamic (loaded from a
 * .so), the shared library is closed using the double-open/double-close
 * trick to undo the initial load performed by class_get().
 *
 * @param name The plugin type string (same as plugin_class::type).
 * @note Silently returns if @p name is not found in the registry.
 */
void class_put(char *name);

/**
 * @brief Look up a plugin class by type name, loading it if necessary.
 *
 * If not already registered (e.g. the shared library has not been loaded
 * yet), attempts to load `LIBDIR/lib<name>.so` via GModule. Loading the
 * .so triggers its constructor attribute function, which calls
 * class_register() to add it to the registry.
 *
 * @param name The plugin type string.
 * @return A plugin_class* (cast to gpointer) on success, or NULL on
 *         failure. The caller must eventually call class_put(name) to
 *         decrement the reference count when the class is no longer needed.
 * @note Increments plugin_class::count on success.
 */
gpointer class_get(char *name);

/* -------------------------------------------------------------------------
 * Plugin instance lifecycle API
 * ------------------------------------------------------------------------- */

/**
 * @brief Allocate a new plugin_instance for the given plugin type.
 *
 * Only allocates; does NOT create any GTK widgets or call the plugin's
 * constructor. Call plugin_start() for that. If the plugin type is not
 * yet loaded, this triggers loading its shared library.
 *
 * @param type Plugin type string; used to look up the plugin_class via
 *             class_get().
 * @return A zero-filled plugin_instance* on success (allocated with
 *         g_malloc0(), size == plugin_class::priv_size), or NULL if the
 *         class cannot be found.
 * @note Caller must eventually call plugin_put() to free the instance.
 *       The class reference count is incremented by the internal
 *       class_get() call.
 */
plugin_instance * plugin_load(char *type);

/**
 * @brief Free a plugin_instance and decrement its class's reference count.
 *
 * @param this The plugin_instance to free. Must have been allocated by
 *             plugin_load(). Must NOT be used after this call.
 * @note Does NOT call the plugin's destructor or destroy any GTK widgets.
 *       Call plugin_stop() first to do that.
 */
void plugin_put(plugin_instance *this);

/**
 * @brief Create the plugin's GTK widget hierarchy and invoke its constructor.
 *
 * For visible plugins (`class->invisible == 0`):
 *   - Creates a GtkBgbox (`this->pwid`).
 *   - Names the widget after the plugin type (for CSS/RC targeting).
 *   - Packs pwid into `panel->box` with this->expand, padding, border.
 *   - If the panel is transparent, sets BG_INHERIT background on pwid.
 *   - Connects the "button-press-event" signal to the panel's handler.
 *   - Shows the widget.
 *
 * For invisible plugins (`class->invisible == 1`):
 *   - Creates a hidden GtkVBox placeholder and packs it into `panel->box`.
 *   - The placeholder is hidden immediately; it exists only to maintain
 *     the child ordering index within the box.
 *
 * Then calls `class->constructor(this)` for both visible and invisible
 * plugins. If the constructor returns 0 (failure), pwid is destroyed and
 * 0 is returned.
 *
 * @param this A plugin_instance initialised by plugin_load(), with panel,
 *             expand, padding, and border fields set by the caller before
 *             calling here.
 * @return 1 on success, 0 on failure (widget is destroyed on failure).
 * @note pwid's floating reference is sunk by gtk_box_pack_start; the
 *       GtkBox (`panel->box`) then holds the sole reference. plugin_stop()
 *       destroys it.
 */
int plugin_start(plugin_instance *this);

/**
 * @brief Tear down a running plugin instance.
 *
 * Order of operations:
 *   -# `destructor(this)` -- plugin cleans up.
 *   -# `plug_num--` -- update panel plugin count.
 *   -# `gtk_widget_destroy()` -- remove widget from panel.
 *
 * @param this A running plugin_instance (previously started with
 *             plugin_start()). After this call returns, `this->pwid` is
 *             invalid. The caller should follow with plugin_put(this) to
 *             free the instance struct.
 */
void plugin_stop(plugin_instance *this);

/**
 * @brief Fallback "configuration not implemented" widget.
 *
 * Used when plugin_class::edit_config is NULL.
 *
 * @param pl The plugin_instance whose name/prefix are shown in the message.
 * @return A newly created GtkVBox containing a GtkLabel; caller owns the
 *         reference (the floating reference is sunk by the container that
 *         receives it).
 * @warning This declaration's name does not match the implementation in
 *          plugin.c (which defines `default_plugin_edit_config`, without
 *          `_instance_`) -- a silent linkage mismatch. Any caller of this
 *          declared function will get a linker error or link to an
 *          unintended symbol.
 */
GtkWidget *default_plugin_instance_edit_config(plugin_instance *pl);

/**
 * @brief Register a plugin_class in the global class registry.
 *
 * Called by the `PLUGIN` macro's constructor attribute function. Adds @p p
 * to the hash table keyed by `p->type`. Sets `p->dynamic` based on whether
 * `the_panel` has been initialised.
 *
 * @param p The plugin_class to register.
 * @warning Calls `exit(1)` if a class with the same type name is already
 *          registered -- there is no recovery path.
 */
extern void class_register(plugin_class *p);

/**
 * @brief Remove a plugin_class from the global class registry.
 *
 * Called by the `PLUGIN` macro's destructor attribute function. Removes
 * @p p from the hash table by `p->type`, and destroys the hash table
 * itself if it becomes empty.
 *
 * @param p The plugin_class to unregister.
 * @note Logs an error if @p p is not found in the registry.
 */
extern void class_unregister(plugin_class *p);

/**
 * @brief Static pointer to this plugin's class descriptor, used by the
 *        auto-registration machinery below.
 *
 * When a plugin .c file `#define`s `PLUGIN` before including plugin.h,
 * this whole block is compiled in. The plugin must set `class_ptr` to
 * point at its own plugin_class descriptor:
 * @code
 *   #define PLUGIN
 *   #include "plugin.h"
 *   static plugin_class myclass = { ... };
 *   static plugin_class *class_ptr = &myclass;
 * @endcode
 *
 * `ctor()`/`dtor()` below are `__attribute__((constructor/destructor))`
 * functions that call class_register()/class_unregister() automatically
 * when the shared library is dlopen'd/dlclose'd.
 *
 * @warning class_ptr is declared here, then re-declared by the plugin as
 *          a static pointer to its own class -- the two declarations must
 *          be compatible (both `static plugin_class*`). If a plugin
 *          forgets to set class_ptr, the constructor registers NULL,
 *          causing undefined behaviour.
 */
#ifdef PLUGIN
static plugin_class *class_ptr;
static void ctor(void) __attribute__ ((constructor));
static void ctor(void) { class_register(class_ptr); }       /**< Called on dlopen. */
static void dtor(void) __attribute__ ((destructor));
static void dtor(void) { class_unregister(class_ptr); }     /**< Called on dlclose. */
#endif

#endif /* PLUGIN_H */
