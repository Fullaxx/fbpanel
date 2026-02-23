/*
 * run.c -- Asynchronous process execution for fbpanel plugins.
 *
 * Two entry points:
 *   run_app()      - launches a shell command string (via g_spawn_command_line_async)
 *   run_app_argv() - launches a command from an argv array (via g_spawn_async)
 *
 * Both display a GTK error dialog on failure and return immediately on success.
 * Callers do not need to wait for or reap child processes.
 *
 * See run.h for the public API documentation.
 */
#include "run.h"
#include "dbg.h"

/*
 * run_app -- spawn a shell command string asynchronously.
 *
 * Wraps g_spawn_command_line_async(), which uses /bin/sh -c internally.
 * Shows a GtkMessageDialog on spawn failure and frees the GError.
 * Silently returns (no error) if cmd is NULL.
 *
 * Memory: the GError is allocated by GLib on failure and freed here.
 * The cmd string is not modified or owned.
 */
void
run_app(gchar *cmd)
{
    GError *error = NULL;

    ENTER;
    if (!cmd)          // nothing to do if no command given
        RET();

    if (!g_spawn_command_line_async(cmd, &error))
    {
        /* Show the spawn error to the user in a modal dialog */
        GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));   // blocks until user closes
        gtk_widget_destroy(dialog);           // clean up the dialog widget
        g_error_free(error);                  // release the GError struct
    }
    RET();
}


/*
 * run_app_argv -- spawn a command from an argv array asynchronously.
 *
 * Uses g_spawn_async() with:
 *   G_SPAWN_DO_NOT_REAP_CHILD: child is not auto-reaped; caller should
 *       add a g_child_watch_add() to avoid zombies (currently not done —
 *       see BUG note in run.h).
 *   G_SPAWN_SEARCH_PATH: searches PATH for argv[0].
 *   G_SPAWN_STDOUT_TO_DEV_NULL: child stdout is discarded.
 *
 * Parameters:
 *   argv - NULL-terminated string array; argv[0] is the executable.
 *
 * Returns: GPid of the child (always valid even if == 0 on some platforms),
 *          or 0 on error.  On error, shows a GTK dialog and frees GError.
 *
 * Memory: GError allocated by GLib on failure; freed here.
 *         argv ownership stays with the caller; not modified.
 */
GPid
run_app_argv(gchar **argv)
{
    GError *error = NULL;
    // Flags: don't reap child, search PATH, silence stdout
    GSpawnFlags flags = G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH;
    GPid pid;

    ENTER;
    flags |= G_SPAWN_STDOUT_TO_DEV_NULL;   // discard child stdout
    if (!g_spawn_async(NULL,               // inherit parent's working dir
                       argv,               // NULL-terminated command+args
                       NULL,               // inherit parent's environment
                       flags,
                       NULL, NULL,         // no child setup function
                       &pid,              // receives child PID on success
                       &error)) {
        /* Spawn failed — show error dialog */
        GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "%s", error->message);
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_error_free(error);
    }

    RET(pid);
}
