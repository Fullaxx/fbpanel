
#ifndef PLUGIN_H
#define PLUGIN_H

/*
 * plugin.h -- Public interface for the fbpanel plugin subsystem.
 *
 * Overview
 * --------
 * fbpanel supports dynamically loaded plugins (shared libraries) as well as
 * plugins compiled directly into the panel binary ("static" plugins).
 *
 * Each plugin type is described by a plugin_class struct, which acts as a
 * vtable plus metadata.  At most one plugin_class exists per plugin type
 * name in the global class registry (class_ht, a GHashTable in plugin.c).
 *
 * Each loaded instance of a plugin is described by a plugin_instance struct.
 * Multiple instances of the same type may coexist; they all share a pointer
 * to the same plugin_class.
 *
 * Registration lifecycle:
 *   Static plugins:
 *     - Registered via the PLUGIN macro at library-load time (__attribute__
 *       ((constructor))).
 *     - Unregistered at library-unload time (__attribute__ ((destructor))).
 *
 *   Dynamic plugins (shared libraries):
 *     - Loaded on demand by class_get() using GModule (dlopen wrapper).
 *     - The shared library's constructor function calls class_register().
 *     - class_put() decrements a reference count and, when it reaches zero,
 *       opens the module a second time then closes it twice to undo the
 *       initial open (a workaround for GModule's lack of a "close once"
 *       without a matching open).
 *
 * Memory ownership:
 *   - plugin_class structs are allocated and owned by the plugin itself
 *     (typically a static variable inside the plugin's .so).
 *   - plugin_instance structs are allocated by plugin_load() with g_malloc0()
 *     and freed by plugin_put() with g_free().  The embedded plugin_class*
 *     pointer is non-owning (do not free it via the instance).
 *   - The GtkWidget* pwid inside plugin_instance is added to the panel's
 *     GtkBox container in plugin_start(); the container owns the widget
 *     reference.  plugin_stop() calls gtk_widget_destroy(pwid) to remove it.
 */

#include <gmodule.h>  // GModule for dynamic library loading


#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <stdio.h>
#include "panel.h"   // panel struct (the main panel state)

/* Forward declaration; defined below. */
struct _plugin_instance;

/*
 * plugin_class -- descriptor / vtable for a plugin type.
 *
 * One of these exists per plugin type name.  For dynamic plugins the fields
 * that are marked "pointers to data within loaded dll" point directly into
 * the loaded shared library's read-only data segment; they become invalid
 * if the library is unloaded.
 *
 * Fields:
 *   fname       - (unused/reserved) file name of the shared library; NULL for
 *                 static plugins.  Not populated or freed by plugin.c.
 *   count       - reference count of active plugin_instance objects of this
 *                 type.  Incremented by class_get(), decremented by class_put().
 *                 When count reaches zero for a dynamic plugin, the .so is
 *                 closed.
 *   gmodule     - handle to the GModule (shared library) for dynamic plugins;
 *                 NULL for static (built-in) plugins.
 *                 NOTE: this field is declared but never populated by plugin.c;
 *                 class_get() does not store the GModule handle, so the .so
 *                 cannot be individually closed via this pointer -- it relies
 *                 on the double-open/double-close trick in class_put() instead.
 *   dynamic     - 1 if the plugin was loaded dynamically after panel startup;
 *                 0 if it was a static (built-in) plugin registered before
 *                 the_panel was initialised.
 *   invisible   - 1 if the plugin has no visible widget; such plugins get a
 *                 hidden GtkVBox as a placeholder so they occupy a slot in the
 *                 panel's box (preserving child ordering).
 *   type        - short ASCII identifier string, e.g. "taskbar", "clock".
 *                 Used as the key in the class registry hash table.
 *                 Points into the plugin's own data; NOT g_free'd by plugin.c.
 *   name        - human-readable display name, e.g. "Task Bar".
 *                 Points into the plugin's own data; NOT g_free'd by plugin.c.
 *   version     - version string of the plugin.
 *   description - one-line description of the plugin.
 *   priv_size   - size in bytes of the plugin_instance subclass struct.
 *                 plugin_load() allocates this many bytes with g_malloc0().
 *                 Must be >= sizeof(plugin_instance).
 *
 * Virtual functions (all non-NULL for a fully implemented plugin):
 *   constructor - called by plugin_start() after the pwid widget has been
 *                 created and added to the panel.  Should complete plugin
 *                 initialisation.  Returns non-zero on success, 0 on failure.
 *   destructor  - called by plugin_stop() before pwid is destroyed.  Must
 *                 release all plugin-owned resources (timers, signals, etc.).
 *                 Must NOT call gtk_widget_destroy(this->pwid); plugin_stop()
 *                 does that after destructor returns.
 *   save_config - serialises the plugin's configuration to fp (an open FILE*).
 *                 May be NULL if the plugin has no persistent configuration.
 *   edit_config - returns a newly-created GtkWidget* tree for the plugin's
 *                 configuration UI (to be embedded in a preferences dialog).
 *                 Caller owns the returned widget.  May be NULL; in that case
 *                 default_plugin_edit_config() is used as fallback.
 */
typedef struct {
    /* common */
    char *fname;       // reserved; not used in current codebase
    int count;         // active instance reference count
    GModule *gmodule;  // GModule handle (always NULL; never set by plugin.c)

    int dynamic : 1;   // 1 = loaded as .so after panel start; 0 = static built-in
    int invisible : 1; // 1 = no visible widget; uses hidden placeholder

    /* these fields are pointers to the data within loaded dll */
    char *type;        // unique ASCII type identifier; hash-table key
    char *name;        // human-readable name
    char *version;     // plugin version string
    char *description; // one-line description
    int priv_size;     // total allocation size for plugin_instance subclass

    /* virtual function table */
    int (*constructor)(struct _plugin_instance *this);       // initialise plugin instance
    void (*destructor)(struct _plugin_instance *this);       // tear down plugin instance
    void (*save_config)(struct _plugin_instance *this, FILE *fp); // serialise config
    GtkWidget *(*edit_config)(struct _plugin_instance *this); // build config UI widget
} plugin_class;

/*
 * PLUGIN_CLASS(class) -- convenience cast from void* or generic pointer to
 * plugin_class*.  Used in plugin code to avoid explicit casts.
 */
#define PLUGIN_CLASS(class) ((plugin_class *) class)

/*
 * plugin_instance -- per-instance state for a running plugin.
 *
 * Allocated by plugin_load() as a zero-filled block of pc->priv_size bytes.
 * The first sizeof(plugin_instance) bytes are this struct; additional bytes
 * are the plugin-specific "private" data (a poor-man's C inheritance trick).
 * Plugins cast (plugin_instance*) to their own larger struct type to access
 * their private fields.
 *
 * Freed by plugin_put() via g_free().
 *
 * Fields:
 *   class   - non-owning pointer to the shared plugin_class descriptor.
 *             Valid for the lifetime of the plugin_instance.
 *   panel   - non-owning pointer to the panel that hosts this plugin.
 *   xc      - parsed configuration node for this plugin instance (from xconf).
 *             Ownership follows xconf conventions; not managed by plugin.c.
 *   pwid    - the GtkWidget* container widget for this plugin's UI.
 *             For visible plugins: a GtkBgbox added to panel->box.
 *             For invisible plugins: a hidden GtkVBox placeholder.
 *             The panel's GtkBox container holds a reference to pwid.
 *             plugin_stop() calls gtk_widget_destroy(pwid) to release it.
 *             Plugins may add children to pwid but must not destroy it directly.
 *   expand  - whether the plugin's pwid expands to fill extra panel space
 *             (passed as the expand argument to gtk_box_pack_start).
 *   padding - extra pixels of space between this plugin and its neighbours
 *             (the padding argument to gtk_box_pack_start).
 *   border  - inner border width in pixels set on the pwid container
 *             (via gtk_container_set_border_width).
 */
typedef struct _plugin_instance{
    plugin_class *class;   // vtable + metadata; shared across all instances of this type
    panel        *panel;   // owning panel; non-owning pointer
    xconf        *xc;      // configuration subtree for this instance
    GtkWidget    *pwid;    // root container widget; owned by the panel's GtkBox
    int           expand;  // GTK expand flag for gtk_box_pack_start
    int           padding; // inter-plugin pixel padding
    int           border;  // gtk_container border_width in pixels
} plugin_instance;

/* -------------------------------------------------------------------------
 * Plugin class registry API
 * ------------------------------------------------------------------------- */

/*
 * class_put:
 *
 * Decrements the reference count for the plugin class named 'name'.
 * If the count drops to zero AND the plugin is dynamic (loaded from a .so),
 * the shared library is closed using the double-open/double-close trick to
 * undo the initial load performed by class_get().
 *
 * Parameters:
 *   name - the plugin type string (same as plugin_class::type).
 *
 * Note: silently returns if 'name' is not found in the registry.
 */
void class_put(char *name);

/*
 * class_get:
 *
 * Looks up a plugin class by type name.  If not already registered (e.g.,
 * the shared library has not been loaded yet), attempts to load
 * LIBDIR/lib<name>.so via GModule.  Loading the .so triggers its constructor
 * attribute function, which calls class_register() to add it to the registry.
 *
 * Parameters:
 *   name - the plugin type string.
 *
 * Returns: a plugin_class* (cast to gpointer) on success, or NULL on failure.
 *          The caller must eventually call class_put(name) to decrement the
 *          reference count when the class is no longer needed.
 *
 * Ref-count: increments plugin_class::count on success.
 */
gpointer class_get(char *name);

/* -------------------------------------------------------------------------
 * Plugin instance lifecycle API
 * ------------------------------------------------------------------------- */

/*
 * plugin_load:
 *
 * Allocates a new plugin_instance for the given plugin type.
 *
 * Parameters:
 *   type - plugin type string; used to look up the plugin_class via class_get.
 *
 * Returns: a zero-filled plugin_instance* on success (allocated with g_malloc0,
 *          size == plugin_class::priv_size), or NULL if the class cannot be found.
 *
 * Memory: caller must eventually call plugin_put() to free the instance.
 *         The class reference count is incremented by the internal class_get() call.
 *
 * Note: this function only allocates; it does NOT create any GTK widgets or
 *       call the plugin's constructor.  Call plugin_start() for that.
 */
/* if plugin_instance is external it will load its dll */
plugin_instance * plugin_load(char *type);

/*
 * plugin_put:
 *
 * Frees the plugin_instance struct and decrements the reference count of the
 * associated plugin_class (which may unload the shared library).
 *
 * Parameters:
 *   this - the plugin_instance to free.  Must have been allocated by plugin_load().
 *          Must NOT be used after this call.
 *
 * Note: plugin_put() does NOT call the plugin's destructor or destroy any GTK
 *       widgets.  Call plugin_stop() first to do that.
 */
void plugin_put(plugin_instance *this);

/*
 * plugin_start:
 *
 * Creates the plugin's GTK widget hierarchy and invokes its constructor.
 *
 * For visible plugins (class->invisible == 0):
 *   - Creates a GtkBgbox (this->pwid).
 *   - Names the widget after the plugin type (for CSS/RC targeting).
 *   - Packs pwid into panel->box with this->expand, padding, border.
 *   - If the panel is transparent, sets BG_INHERIT background on pwid.
 *   - Connects the "button-press-event" signal to the panel's handler.
 *   - Shows the widget.
 *
 * For invisible plugins (class->invisible == 1):
 *   - Creates a hidden GtkVBox placeholder and packs it into panel->box.
 *   - The placeholder is hidden immediately; it exists only to maintain
 *     the child ordering index within the box.
 *
 * Then calls class->constructor(this) for both visible and invisible plugins.
 * If the constructor returns 0 (failure), pwid is destroyed and 0 is returned.
 *
 * Parameters:
 *   this - a plugin_instance initialised by plugin_load() with panel, expand,
 *          padding, and border fields set by the caller before calling here.
 *
 * Returns: 1 on success, 0 on failure (widget is destroyed on failure).
 *
 * Widget ownership:
 *   pwid's floating reference is sunk by gtk_box_pack_start; the GtkBox
 *   (panel->box) then holds the sole reference.  plugin_stop() destroys it.
 */
int plugin_start(plugin_instance *this);

/*
 * plugin_stop:
 *
 * Tears down a running plugin instance.
 *
 * Calls class->destructor(this) to allow the plugin to release its resources,
 * decrements panel->plug_num, then calls gtk_widget_destroy(this->pwid) to
 * remove the widget from the panel and drop the container's reference.
 *
 * Parameters:
 *   this - a running plugin_instance (previously started with plugin_start).
 *          After plugin_stop() returns, this->pwid is invalid.  The caller
 *          should follow with plugin_put(this) to free the instance struct.
 *
 * Order of operations:
 *   1. destructor(this)       -- plugin cleans up
 *   2. plug_num--             -- update panel plugin count
 *   3. gtk_widget_destroy()   -- remove widget from panel
 */
void plugin_stop(plugin_instance *this);

/*
 * default_plugin_instance_edit_config:
 *
 * NOTE: this declaration has a different name from the implementation in
 * plugin.c (which defines default_plugin_edit_config, without "_instance_").
 * This is a silent linkage mismatch -- the declaration here will not resolve
 * to the actual implementation, and any caller of this declared function will
 * get a linker error or link to an unintended symbol.
 *
 * Returns a GtkWidget* tree with a fallback "configuration not implemented"
 * message.  Used when plugin_class::edit_config is NULL.
 *
 * Parameters:
 *   pl - the plugin_instance whose name/prefix are shown in the message.
 *
 * Returns: a newly created GtkVBox containing a GtkLabel; caller owns the ref
 *          (floating reference sunk by the container that receives it).
 */
GtkWidget *default_plugin_instance_edit_config(plugin_instance *pl);

/*
 * class_register / class_unregister:
 *
 * Called by the PLUGIN macro's constructor/destructor attribute functions.
 * Registers or removes a plugin_class from the global class registry.
 *
 * class_register:
 *   Adds p to the hash table keyed by p->type.  Calls exit(1) if a class
 *   with the same type name is already registered (no recovery path).
 *   Sets p->dynamic based on whether the_panel has been initialised.
 *
 * class_unregister:
 *   Removes p from the hash table by p->type.  Logs an error if not found.
 *   Destroys the hash table itself if it becomes empty.
 */
extern void class_register(plugin_class *p);
extern void class_unregister(plugin_class *p);

/*
 * PLUGIN macro -- boilerplate for plugin shared libraries.
 *
 * When a plugin .c file #defines PLUGIN before including plugin.h, this block
 * is compiled in.  It declares:
 *   - class_ptr: a static plugin_class* that the plugin must initialise with
 *     a pointer to its plugin_class descriptor.
 *   - ctor(): __attribute__((constructor)) function that calls class_register
 *     when the shared library is loaded (dlopen).
 *   - dtor(): __attribute__((destructor)) function that calls class_unregister
 *     when the shared library is unloaded (dlclose).
 *
 * Usage in a plugin .c file:
 *   #define PLUGIN
 *   #include "plugin.h"
 *   static plugin_class myclass = { ... };
 *   static plugin_class *class_ptr = &myclass;
 *
 * NOTE: class_ptr is declared here as a static pointer but then re-declared
 * by the plugin as a static pointer-to-its-own-class.  The two declarations
 * must be compatible (both static plugin_class*); if a plugin forgets to set
 * class_ptr the constructor will register NULL, causing undefined behaviour.
 */
#ifdef PLUGIN
static plugin_class *class_ptr;                              // plugin must set this
static void ctor(void) __attribute__ ((constructor));
static void ctor(void) { class_register(class_ptr); }       // called on dlopen
static void dtor(void) __attribute__ ((destructor));
static void dtor(void) { class_unregister(class_ptr); }     // called on dlclose
#endif

#endif /* PLUGIN_H */
