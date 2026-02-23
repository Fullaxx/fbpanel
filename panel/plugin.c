
/*
 * plugin.c -- Plugin class registry and instance lifecycle management for fbpanel.
 *
 * This file implements:
 *
 *   Class registry  (class_ht, class_register, class_unregister, class_get,
 *                    class_put)
 *     A process-global GHashTable maps plugin type-name strings to
 *     plugin_class* descriptors.  Static plugins register themselves at startup
 *     via __attribute__((constructor)); dynamic plugins are loaded on demand
 *     by class_get() using GModule (dlopen).
 *
 *   Instance lifecycle  (plugin_load, plugin_put, plugin_start, plugin_stop)
 *     plugin_load() allocates a zero-filled plugin_instance (size given by
 *     plugin_class::priv_size) and links it to its class.
 *     plugin_start() creates the GTK widget and calls the plugin constructor.
 *     plugin_stop() calls the destructor and destroys the widget.
 *     plugin_put() frees the instance struct and decrements the class refcount.
 *
 *   Default configuration UI  (default_plugin_edit_config)
 *     Returns a "not implemented" message widget used when a plugin has no
 *     edit_config vfunc.
 *
 * Global state:
 *   class_ht     - the plugin class hash table; NULL until the first
 *                  class_register() call and destroyed again when empty.
 *   the_panel    - extern; the singleton panel instance.  Its non-NULL-ness
 *                  at registration time determines whether a plugin is
 *                  "dynamic" (loaded after startup) or "static" (built-in).
 */

#include "plugin.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf-xlib/gdk-pixbuf-xlib.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdlib.h>  // exit()



#include "misc.h"
#include "bg.h"
#include "gtkbgbox.h"


//#define DEBUGPRN
#include "dbg.h"  // ENTER/RET/DBG/ERR tracing macros

/*
 * the_panel -- singleton panel instance declared in main panel code.
 * We use it here only to determine whether a registering plugin is "dynamic"
 * (registered after the panel is up) or "static" (registered at init time).
 */
extern panel *the_panel;


/**************************************************************/

/*
 * class_ht -- global plugin class registry.
 *
 * Maps (char* type) -> (plugin_class*).
 * Created lazily on first class_register() call; destroyed in
 * class_unregister() when it becomes empty.
 * Key strings are the plugin_class::type pointers (not duplicated);
 * they must remain valid for the lifetime of the entry.
 * Values are plugin_class* pointers (not owned; plugin owns the struct).
 */
static GHashTable *class_ht;


/*
 * class_register:
 *
 * Adds the plugin class descriptor *p to the global registry.
 *
 * Parameters:
 *   p - non-NULL plugin_class* whose ->type field is used as the hash key.
 *       The type string must remain valid for as long as the class is
 *       registered (it is used as a key without being duplicated).
 *       The struct itself must remain valid; plugin.c does not copy it.
 *
 * Side effects:
 *   - Creates class_ht on first call.
 *   - Sets p->dynamic = (the_panel != NULL):
 *       - 0 for static built-in plugins (registered before the panel window).
 *       - 1 for plugins loaded from .so files after the panel is running.
 *   - Calls exit(1) -- FATAL -- if a class with the same type is already
 *     registered.  There is no error-recovery path.
 *
 * Thread safety: not thread-safe (no locking on class_ht).
 */
void
class_register(plugin_class *p)
{
    ENTER;
    if (!class_ht) {
        // First registration: create the hash table.
        // g_str_hash / g_str_equal treat the keys as NUL-terminated strings.
        class_ht = g_hash_table_new(g_str_hash, g_str_equal);
        DBG("creating class hash table\n");
    }
    DBG("registering %s\n", p->type);

    // Reject duplicate type names; this is a hard error because duplicate
    // plugin names would silently shadow each other and confuse the registry.
    if (g_hash_table_lookup(class_ht, p->type)) {
        ERR("Can't register plugin %s. Such name already exists.\n", p->type);
        exit(1);  // no recovery; terminates the process
    }

    // Mark the plugin as dynamic (loaded after panel startup) or static.
    // the_panel is NULL during early initialisation (static plugin registration)
    // and non-NULL once the panel window has been created.
    p->dynamic = (the_panel != NULL); /* dynamic modules register after main */

    // Insert; keys and values are not copied -- pointers into the plugin's data.
    g_hash_table_insert(class_ht, p->type, p);
    RET();
}

/*
 * class_unregister:
 *
 * Removes the plugin class descriptor *p from the global registry.
 *
 * Parameters:
 *   p - the plugin_class previously passed to class_register().
 *       p->type is used as the lookup key.
 *
 * Side effects:
 *   - Logs an error (but does NOT abort) if p->type is not found.
 *   - If the registry becomes empty after removal, the hash table is
 *     destroyed and class_ht is set to NULL (so the next registration
 *     re-creates it).
 *
 * Note: the plugin_class struct itself is NOT freed here (it is owned by
 * the plugin, typically as a static variable inside the .so).
 */
void
class_unregister(plugin_class *p)
{
    ENTER;
    DBG("unregistering %s\n", p->type);

    // Remove the entry; g_hash_table_remove returns FALSE if key not found.
    if (!g_hash_table_remove(class_ht, p->type)) {
        ERR("Can't unregister plugin %s. No such name\n", p->type);
        // Bug: function continues even on error; no RET() here guards the
        // g_hash_table_size() call below, which is safe because class_ht is
        // not NULL (we reached the remove call), but the empty-table cleanup
        // logic below may trigger incorrectly after a failed removal.
    }

    // If the registry is now empty, tear it down completely.
    if (!g_hash_table_size(class_ht)) {
        DBG("dwstroying class hash table\n");  // note: "dwstroying" is a typo
        g_hash_table_destroy(class_ht);
        class_ht = NULL;
    }
    RET();
}

/*
 * class_put:
 *
 * Decrements the reference count for the plugin class named 'name'.
 * If the count reaches zero AND the class is dynamic, closes the shared
 * library using the double-open/double-close trick to undo the initial open
 * performed by class_get().
 *
 * Parameters:
 *   name - the plugin type string.
 *
 * Design note -- the double-open/double-close trick:
 *   GModule (dlopen) is reference-counted.  class_get() opened the .so once.
 *   To close it we need to call g_module_close() twice:
 *     1st close: the newly opened handle (from within class_put).
 *     2nd close: conceptually undoes the original open from class_get().
 *   This works because GModule tracks the open count per path: opening the
 *   same path twice gives a new handle but increments the internal count;
 *   two closes decrement it back to zero and trigger unloading (and thus the
 *   __attribute__((destructor)) which calls class_unregister).
 *
 *   WARNING: this is fragile.  If class_get() is called more than once for
 *   a dynamic plugin (count > 1 at decrement time), class_put() will NOT
 *   attempt a close; the .so stays loaded until all instances are released.
 *   However, the close attempt happens when count==0 regardless of whether
 *   the double-open is correct at that moment.  If anything else has opened
 *   or closed the same .so the count will be wrong.
 */
void
class_put(char *name)
{
    GModule *m;
    gchar *s;
    plugin_class *tmp;

    ENTER;
    DBG("%s\n", name);

    // If the registry is empty or the name is not found, nothing to do.
    if (!(class_ht && (tmp = g_hash_table_lookup(class_ht, name))))
        RET();

    tmp->count--;  // decrement the active-instance reference count

    // If there are still active instances, or if the plugin is static (not
    // dynamically loaded), there is nothing further to do.
    if (tmp->count || !tmp->dynamic)
        RET();

    // The last instance of a dynamic plugin was released; close the .so.
    s = g_strdup_printf(LIBDIR "/lib%s.so", name);  // reconstruct the .so path
    DBG("loading module %s\n", s);

    // Open the module a second time to get a fresh GModule handle.
    m = g_module_open(s, G_MODULE_BIND_LAZY);
    g_free(s);  // path string no longer needed

    if (m) {
        /* Close it twice to undo initial open in class_get */
        // 1st close: the handle we just opened above.
        g_module_close(m);
        // 2nd close: undoes the handle opened by class_get(); the library's
        // internal ref-count should now reach zero, triggering unload and
        // the __attribute__((destructor)) in the plugin which calls
        // class_unregister().
        g_module_close(m);
    }
    // If g_module_open() fails (m == NULL), the .so cannot be found/opened;
    // we silently leave the library loaded.  No error is reported here, which
    // means a failed unload is silently ignored.
    RET();
}

/*
 * class_get:
 *
 * Looks up a plugin class by type name.  If not already in the registry,
 * attempts to load LIBDIR/lib<name>.so via GModule.
 *
 * Parameters:
 *   name - the plugin type string (e.g., "taskbar", "clock").
 *
 * Returns: (plugin_class*) cast to gpointer on success; NULL on failure.
 *          On success, plugin_class::count has been incremented by 1.
 *          The caller must call class_put(name) when done to balance this.
 *
 * Loading sequence:
 *   1. Check class_ht for the name (fast path; .so already loaded).
 *   2. If not found, construct the .so path and call g_module_open().
 *      g_module_open() loads the library and runs its constructor functions,
 *      which call class_register() to add the class to class_ht.
 *   3. After the load attempt, check class_ht again; if the class is now
 *      present (registration succeeded), return it.
 *   4. If still not found, log the GModule error and return NULL.
 *
 * NOTE: the GModule handle returned by g_module_open() is NOT stored anywhere
 * (plugin_class::gmodule is never set here).  The library remains open until
 * the double-close in class_put() or until process exit.  This means there is
 * no way to forcibly unload a module from outside class_put().
 *
 * NOTE: if g_module_open() succeeds but the constructor does NOT call
 * class_register() (e.g., class_ptr is NULL), the function falls through to
 * the ERR path and returns NULL, but the library remains loaded (leak).
 */
gpointer
class_get(char *name)
{
    GModule *m;
    plugin_class *tmp;
    gchar *s;

    ENTER;
    DBG("%s\n", name);

    // Fast path: class already registered (either static or previously loaded).
    if (class_ht && (tmp = g_hash_table_lookup(class_ht, name))) {
        DBG("found\n");
        tmp->count++;  // increment active-instance count
        RET(tmp);
    }

    // Slow path: attempt to load the shared library for this plugin type.
    s = g_strdup_printf(LIBDIR "/lib%s.so", name);  // e.g., "/usr/lib/libclock.so"
    DBG("loading module %s\n", s);
    m = g_module_open(s, G_MODULE_BIND_LAZY);  // LAZY = resolve symbols on use
    g_free(s);  // path string no longer needed after open

    if (m) {
        // The library loaded and its constructor (ctor()) ran class_register().
        // Check the registry again for the newly registered class.
        if (class_ht && (tmp = g_hash_table_lookup(class_ht, name))) {
            DBG("found\n");
            tmp->count++;  // increment active-instance count
            RET(tmp);
        }
        // Library loaded but class not found -- constructor did not register it,
        // or registered under a different name.  The GModule handle 'm' is
        // NOT closed here, causing a module leak.
    }

    // Either the module failed to open, or registration didn't happen.
    ERR("%s\n", g_module_error());  // log the GModule error message
    RET(NULL);
}



/**************************************************************/

/*
 * plugin_load:
 *
 * Allocates a new plugin_instance for the plugin type identified by 'type'.
 *
 * Parameters:
 *   type - plugin type string; must match a registered plugin_class::type.
 *
 * Returns: a zero-filled plugin_instance* (actually pc->priv_size bytes, which
 *          is >= sizeof(plugin_instance)) with pp->class set; or NULL if the
 *          class cannot be found/loaded.
 *
 * Memory:
 *   Allocated with g_malloc0(); caller must call plugin_put() to free.
 *   The internal class_get() call increments plugin_class::count; class_put()
 *   inside plugin_put() will decrement it.
 *
 * Note: only allocates -- does NOT create widgets or call the constructor.
 *       The caller must set pp->panel, pp->expand, pp->padding, pp->border
 *       before calling plugin_start().
 */
plugin_instance *
plugin_load(char *type)
{
    plugin_class *pc = NULL;
    plugin_instance  *pp = NULL;

    ENTER;
    /* nothing was found */
    if (!(pc = class_get(type)))  // increments pc->count on success
        RET(NULL);

    DBG("%s priv_size=%d\n", pc->type, pc->priv_size);

    // Allocate zero-filled memory for the plugin instance (which may be a
    // larger struct in the plugin's own code).  g_malloc0 never returns NULL
    // in GLib (it aborts on OOM), but the g_return_val_if_fail below was
    // presumably left as a defensive check.
    pp = g_malloc0(pc->priv_size);
    g_return_val_if_fail (pp != NULL, NULL);  // defensive; g_malloc0 never returns NULL

    pp->class = pc;  // link back to the class descriptor (non-owning)
    RET(pp);
}


/*
 * plugin_put:
 *
 * Frees the plugin_instance struct and decrements the class reference count.
 *
 * Parameters:
 *   this - plugin_instance allocated by plugin_load().  Must NOT be used
 *          after this call.  Must have been stopped with plugin_stop() first
 *          (otherwise pwid and any plugin-owned resources are leaked).
 *
 * Order:
 *   1. Save this->class->type before freeing (g_free invalidates the struct).
 *   2. g_free(this) -- releases the instance memory.
 *   3. class_put(type) -- decrements refcount; may unload the .so.
 *
 * Note: this does NOT call the plugin destructor or destroy any widget.
 *       The caller is responsible for calling plugin_stop() before plugin_put().
 */
void
plugin_put(plugin_instance *this)
{
    gchar *type;

    ENTER;
    type = this->class->type;  // save type name before freeing 'this'
    g_free(this);              // release the instance allocation
    class_put(type);           // decrement class refcount (may unload .so)
    RET();
}

/*
 * panel_button_press_event -- forward declaration.
 *
 * Defined elsewhere (panel.c or similar); handles right-click context menus
 * on the panel background.  Declared here so we can connect it as a signal
 * callback on pwid in plugin_start().
 */
gboolean panel_button_press_event(GtkWidget *widget, GdkEventButton *event,
        panel *p);

/*
 * plugin_start:
 *
 * Creates the plugin's GtkWidget hierarchy and invokes its constructor.
 *
 * Parameters:
 *   this - a plugin_instance initialised by plugin_load() with the following
 *          fields already set by the caller:
 *            this->panel    -- the hosting panel
 *            this->expand   -- gtk_box_pack_start expand argument
 *            this->padding  -- gtk_box_pack_start padding argument
 *            this->border   -- container border_width in pixels
 *
 * Returns: 1 on success, 0 on failure.
 *
 * Visible plugin widget lifecycle (class->invisible == 0):
 *   1. gtk_bgbox_new()           -- creates pwid with floating ref.
 *   2. gtk_box_pack_start()      -- sinks floating ref; panel->box owns pwid.
 *   3. gtk_bgbox_set_background()-- sets BG_INHERIT if panel is transparent.
 *   4. g_signal_connect()        -- connects button-press to panel handler.
 *   5. gtk_widget_show(pwid)     -- makes widget visible.
 *   6. class->constructor(this)  -- plugin initialises itself.
 *   7. On constructor failure: gtk_widget_destroy(pwid) removes it from box.
 *
 * Invisible plugin widget lifecycle (class->invisible == 1):
 *   1. gtk_vbox_new()            -- creates hidden placeholder with floating ref.
 *   2. gtk_box_pack_start()      -- sinks floating ref; panel->box owns pwid.
 *   3. gtk_widget_hide()         -- keeps it invisible.
 *   4. class->constructor(this)  -- plugin initialises itself (no widget to show).
 *
 * Note: plug_num is NOT incremented here; it is decremented in plugin_stop().
 *       This asymmetry means plug_num can go negative if plugin_stop() is
 *       called without a corresponding "count-up" somewhere else.
 */
int
plugin_start(plugin_instance *this)
{
    ENTER;

    DBG("%s\n", this->class->type);
    if (!this->class->invisible) {
        // Visible plugin: create a GtkBgbox container.
        this->pwid = gtk_bgbox_new();  // returns floating reference

        // Name the widget to allow GTK RC file theming by plugin type.
        gtk_widget_set_name(this->pwid, this->class->type);

        // Pack into the panel's layout box; gtk_box_pack_start sinks the
        // floating reference -- the box now owns this->pwid.
        gtk_box_pack_start(GTK_BOX(this->panel->box), this->pwid, this->expand,
                TRUE, this->padding);
        DBG("%s expand %d\n", this->class->type, this->expand);

        // Apply the configured inner border.
        gtk_container_set_border_width(GTK_CONTAINER(this->pwid), this->border);

        DBG("here this->panel->transparent = %d\n", this->panel->transparent);
        if (this->panel->transparent) {
            DBG("here g\n");
            // Panel is pseudo-transparent: inherit the panel background's pixmap
            // (BG_INHERIT mode tells X to use the parent window's background).
            gtk_bgbox_set_background(this->pwid, BG_INHERIT,
                    this->panel->tintcolor, this->panel->alpha);
        }
        DBG("here\n");

        // Connect the panel's context-menu handler so right-clicking on any
        // plugin's background area shows the panel menu.
        g_signal_connect (G_OBJECT (this->pwid), "button-press-event",
              (GCallback) panel_button_press_event, this->panel);

        gtk_widget_show(this->pwid);  // make the container visible
        DBG("here\n");
    } else {
        /* create a no-window widget and do not show it it's usefull to have
         * unmaped widget for invisible plugins so their indexes in plugin list
         * are the same as in panel->box. required for children reordering */

        // Invisible plugin: create a hidden VBox placeholder.
        // It occupies a slot in panel->box without displaying anything.
        // This preserves the correspondence between plugins[] indices and box
        // child indices, which the reordering code relies on.
        this->pwid = gtk_vbox_new(TRUE, 0);  // returns floating reference
        gtk_box_pack_start(GTK_BOX(this->panel->box), this->pwid, FALSE,
                TRUE,0);  // sink floating ref; box now owns pwid
        gtk_widget_hide(this->pwid);  // ensure it stays invisible
    }
    DBG("here\n");

    // Invoke the plugin's own constructor.  At this point pwid exists and is
    // packed into the panel.  The constructor should add child widgets to pwid.
    if (!this->class->constructor(this)) {
        DBG("here\n");
        // Constructor failed; destroy the container widget we just created.
        // gtk_widget_destroy removes pwid from the box and drops the box's ref.
        gtk_widget_destroy(this->pwid);
        RET(0);  // signal failure to caller
    }
    RET(1);  // success
}


/*
 * plugin_stop:
 *
 * Tears down a running plugin instance by calling its destructor and
 * destroying its widget.
 *
 * Parameters:
 *   this - a plugin_instance in the "started" state (plugin_start returned 1).
 *          After plugin_stop() returns, this->pwid is invalid (destroyed).
 *
 * Side effects:
 *   1. Calls this->class->destructor(this): the plugin must release all
 *      resources it acquired in its constructor (timers, signal handlers,
 *      child widgets it created, etc.).  The destructor must NOT call
 *      gtk_widget_destroy on this->pwid; plugin_stop() does that next.
 *   2. Decrements this->panel->plug_num.
 *   3. Calls gtk_widget_destroy(this->pwid): removes the widget from the
 *      panel's box, fires the "destroy" signal, and releases the box's
 *      reference.  Any children of pwid are also destroyed recursively.
 *
 * The caller should call plugin_put(this) afterward to free the instance.
 *
 * WARNING: plug_num is decremented here but is never incremented in
 * plugin_start().  The increment must happen in the caller (panel loading
 * code).  This is a structural coupling that is easy to get wrong.
 */
void
plugin_stop(plugin_instance *this)
{
    ENTER;
    DBG("%s\n", this->class->type);
    this->class->destructor(this);  // let the plugin clean up first
    this->panel->plug_num--;         // update the panel's active plugin count
    gtk_widget_destroy(this->pwid); // remove from panel; drops container's ref
    RET();
}


/*
 * default_plugin_edit_config:
 *
 * Fallback configuration UI for plugins that do not implement edit_config.
 * Creates and returns a simple GtkVBox containing a user-facing message
 * explaining how to edit the configuration manually.
 *
 * Parameters:
 *   pl - the plugin_instance for which a config UI was requested.
 *        pl->class->name is used in the message text.
 *
 * Returns: a GtkWidget* (GtkVBox) with a floating reference.  Caller is
 *          responsible for managing the reference (typically by packing it
 *          into a dialog or configuration window which sinks the floating ref).
 *
 * Memory:
 *   msg is a g_strdup_printf'd string; freed before return.
 *   label is owned by vbox (added via gtk_box_pack_end).
 *   vbox is returned with a floating reference.
 *
 * NOTE: this function is defined here as default_plugin_edit_config but
 * declared in plugin.h as default_plugin_instance_edit_config.  The name
 * mismatch means the declaration in the header does not match this definition,
 * creating a potential linkage issue for any translation unit that includes
 * plugin.h and calls the declared name.
 */
GtkWidget *
default_plugin_edit_config(plugin_instance *pl)
{
    GtkWidget *vbox, *label;
    gchar *msg;

    ENTER;
    vbox = gtk_vbox_new(FALSE, 0);  // vertical container; returned with floating ref

    /* XXX: harcoded default profile name */
    // Build the informational message string; includes the plugin name, the
    // default config file path (hardcoded), and the example data directory.
    msg = g_strdup_printf("Graphical '%s' plugin configuration\n is not "
          "implemented yet.\n"
          "Please edit manually\n\t~/.config/fbpanel/default\n\n"
          "You can use as example files in \n\t%s/share/fbpanel/\n"
          "or visit\n"
          "\thttp://fbpanel.sourceforge.net/docs.html", pl->class->name,
          PREFIX);  // PREFIX is a compile-time constant from config.h

    label = gtk_label_new(msg);                         // create label with message
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);    // wrap long lines
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);   // allow copy-paste of the path

    // Pack label into vbox; gtk_box_pack_end sinks label's floating ref.
    gtk_box_pack_end(GTK_BOX(vbox), label, TRUE, TRUE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);  // outer margin

    g_free(msg);  // release the formatted string; label has already copied it

    RET(vbox);  // floating reference; caller must manage
}
