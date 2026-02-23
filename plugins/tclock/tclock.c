/* tclock -- Text Clock plugin for fbpanel.
 *
 * Unlike dclock (which renders pixel-art glyphs), tclock uses a GtkLabel with
 * Pango markup so the clock text honours the GTK theme font and can embed bold
 * or colour tags directly in the format string (e.g. "<b>%R</b>").
 *
 * 2010-04 Jared Minch  < jmminch@sourceforge.net >
 *     Calendar and transparency support
 *     See patch "2981313: Enhancements to 'tclock' plugin" on sf.net
 *
 * Timer: a 1-second g_timeout_add fires clock_update() continuously.
 * Memory: dc->main and dc->clockw are regular GTK child widgets; the parent
 *         container manages their lifetime. Timer handle dc->timer must be
 *         removed in tclock_destructor.
 */

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

/* strftime format for the tooltip -- weekday + locale date */
#define TOOLTIP_FMT    "%A %x"
/* Default 24h Pango-markup format; bold via <b> tag */
#define CLOCK_24H_FMT  "<b>%R</b>"
/* 12h format with no extra markup */
#define CLOCK_12H_FMT  "%I:%M"

/*
 * tclock_priv -- per-instance private state.
 *
 * Memory ownership:
 *   main        -- GtkEventBox; owned by GTK widget tree.
 *   clockw      -- GtkLabel child of main; owned by GTK widget tree.
 *   calendar_window -- GtkWindow popup, or NULL; owned by this struct.
 *                     Must be destroyed when the plugin unloads if non-NULL.
 *   tfmt, cfmt  -- point into xconf storage (not heap); do not free.
 *   action      -- points into xconf storage; do not free.
 *   timer       -- GSource handle from g_timeout_add; 0 when not running.
 */
typedef struct {
    plugin_instance plugin;    /* base class -- must be first member          */
    GtkWidget *main;           /* outer event box catching button events      */
    GtkWidget *clockw;         /* GtkLabel displaying the formatted time      */
    GtkWidget *calendar_window;/* popup calendar, or NULL                     */
    char *tfmt;                /* strftime format for tooltip string          */
    char *cfmt;                /* strftime format for clock face (may include Pango markup) */
    char *action;              /* shell command on click, or NULL             */
    short lastDay;             /* day-of-month when tooltip was last rebuilt  */
    int timer;                 /* g_timeout_add handle; 0 = not running       */
    int show_calendar;         /* non-zero = toggle calendar on click         */
    int show_tooltip;          /* non-zero = show date tooltip                */
} tclock_priv;

/*
 * tclock_create_calendar -- construct and return an undecorated popup calendar.
 *
 * Returns: GtkWidget* (GtkWindow) with a GtkCalendar inside.
 *          Caller takes ownership; destroy with gtk_widget_destroy().
 *
 * The window: undecorated, non-resizable, positioned near the mouse,
 * sticky (all desktops), excluded from taskbar and pager.
 */
static GtkWidget *
tclock_create_calendar(void)
{
    GtkWidget *calendar, *win;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);   // no title bar
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 5);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    gtk_window_set_title(GTK_WINDOW(win), "calendar");
    gtk_window_stick(GTK_WINDOW(win)); // visible on all virtual desktops

    calendar = gtk_calendar_new();
    gtk_calendar_display_options(
        GTK_CALENDAR(calendar),
        GTK_CALENDAR_SHOW_WEEK_NUMBERS | GTK_CALENDAR_SHOW_DAY_NAMES
        | GTK_CALENDAR_SHOW_HEADING);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(calendar));

    return win;
}

/*
 * clock_update -- timer callback; refreshes the clock label and tooltip.
 *
 * Parameters:
 *   data -- gpointer cast of tclock_priv*.
 *
 * Returns: TRUE (always), to keep the g_timeout_add timer alive.
 *
 * Clock face: calls strftime with dc->cfmt; passes the result directly to
 *   gtk_label_set_markup so Pango markup in cfmt (e.g. "<b>%R</b>") is
 *   rendered. This means user-supplied cfmt strings containing literal '<'
 *   characters will break Pango parsing -- see BUG entry.
 *
 * Tooltip: updated only once per day (when detail->tm_mday changes) unless
 *   the calendar popup is open, in which case the tooltip is cleared to avoid
 *   overlap.  The converted UTF-8 string is freed after being passed to GTK.
 *
 * localtime() is not thread-safe but fbpanel is single-threaded so this is OK.
 */
static gint
clock_update(gpointer data)
{
    char output[256]; // strftime output buffer
    time_t now;
    struct tm * detail;
    tclock_priv *dc;
    gchar *utf8;
    size_t rc;

    ENTER;
    g_assert(data != NULL);
    dc = (tclock_priv *)data;

    time(&now);
    detail = localtime(&now); // returns pointer to static buffer
    // Format the clock face; rc == 0 means the buffer was too small or format is empty
    rc = strftime(output, sizeof(output), dc->cfmt, detail) ;
    if (rc) {
        // Set the label; cfmt may include Pango markup so use set_markup
        gtk_label_set_markup (GTK_LABEL(dc->clockw), output) ;
    }

    if (dc->show_tooltip) {
        if (dc->calendar_window) {
            // Calendar is open: clear tooltip to prevent overlap
            gtk_widget_set_tooltip_markup(dc->main, NULL);
            dc->lastDay = 0; // force tooltip rebuild when calendar closes
        } else {
            // Rebuild tooltip only when the day changes (once per midnight)
            if (detail->tm_mday != dc->lastDay) {
                dc->lastDay = detail->tm_mday;

                rc = strftime(output, sizeof(output), dc->tfmt, detail) ;
                if (rc &&
                    (utf8 = g_locale_to_utf8(output, -1, NULL, NULL, NULL))) {
                    gtk_widget_set_tooltip_markup(dc->main, utf8);
                    g_free(utf8); // free the g_locale_to_utf8 allocation
                }
            }
        }
    }

    RET(TRUE); // keep timer running
}

/*
 * clicked -- button-press-event callback for the clock event box.
 *
 * Parameters:
 *   widget -- the GtkEventBox (dc->main)
 *   event  -- GdkEventButton
 *   dc     -- tclock_priv instance
 *
 * Returns: TRUE (event consumed).
 *
 * Behaviour:
 *   If dc->action is set, runs it asynchronously.
 *   Otherwise, if show_calendar is enabled, toggles the popup calendar.
 *   After toggling the calendar, calls clock_update() immediately to refresh
 *   the tooltip state (clear it while calendar is open).
 *
 * Note: unlike dclock, this handler does NOT propagate Control+RightClick to
 * the panel for the panel context menu.  This means Control+RightClick on the
 * tclock widget does NOT open the panel configuration dialog -- see BUG entry.
 *
 * Signal connection: "button_press_event" on dc->main (if action or show_calendar).
 */
static gboolean
clicked(GtkWidget *widget, GdkEventButton *event, tclock_priv *dc)
{
    ENTER;
    if (dc->action) {
        // Run configured command; errors are silently ignored
        g_spawn_command_line_async(dc->action, NULL);
    } else if (dc->show_calendar) {
        if (dc->calendar_window == NULL)
        {
            // Open the calendar popup
            dc->calendar_window = tclock_create_calendar();
            gtk_widget_show_all(dc->calendar_window);
        }
        else
        {
            // Close the calendar popup
            gtk_widget_destroy(dc->calendar_window);
            // Set to NULL so clock_update() knows the calendar is gone
            dc->calendar_window = NULL;
        }
        // Immediately refresh tooltip (clear while open, restore when closed)
        clock_update(dc);
    }
    RET(TRUE);
}

/*
 * tclock_constructor -- initialise and display the text clock plugin.
 *
 * Parameters:
 *   p -- plugin_instance allocated by the framework.
 *
 * Returns: 1 on success.
 *
 * Configuration keys (via XCG):
 *   TooltipFmt    -- strftime format for tooltip (default TOOLTIP_FMT)
 *   ClockFmt      -- strftime/Pango format for label (default CLOCK_24H_FMT)
 *   Action        -- optional shell command on click
 *   ShowCalendar  -- bool: show popup calendar on click (default TRUE)
 *   ShowTooltip   -- bool: show date tooltip (default TRUE)
 *
 * Widget hierarchy:
 *   p->pwid  (container from framework)
 *     dc->main (GtkEventBox, invisible window)
 *       dc->clockw (GtkLabel, centre-aligned)
 *
 * Signal: "button_press_event" on dc->main -> clicked() (only connected if
 *         action or show_calendar is configured).
 *
 * Timer: dc->timer starts the 1-second periodic update.
 *        Must be cancelled in tclock_destructor.
 */
static int
tclock_constructor(plugin_instance *p)
{
    tclock_priv *dc;

    ENTER;
    dc = (tclock_priv *) p;
    // Set defaults before reading configuration
    dc->cfmt = CLOCK_24H_FMT;
    dc->tfmt = TOOLTIP_FMT;
    dc->action = NULL;
    dc->show_calendar = TRUE;
    dc->show_tooltip = TRUE;
    // Read per-instance configuration overrides
    XCG(p->xc, "TooltipFmt", &dc->tfmt, str);
    XCG(p->xc, "ClockFmt", &dc->cfmt, str);
    XCG(p->xc, "Action", &dc->action, str);
    XCG(p->xc, "ShowCalendar", &dc->show_calendar, enum, bool_enum);
    XCG(p->xc, "ShowTooltip", &dc->show_tooltip, enum, bool_enum);

    // Create an event box as the outer widget (invisible, catches events)
    dc->main = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(dc->main), FALSE);
    // Only hook the click handler if there is something to do on click
    if (dc->action || dc->show_calendar)
        g_signal_connect (G_OBJECT (dc->main), "button_press_event",
              G_CALLBACK (clicked), (gpointer) dc);

    // Create the text label; will be populated by clock_update()
    dc->clockw = gtk_label_new(NULL);

    clock_update(dc); // render once immediately before showing the widget

    // Configure label appearance
    gtk_misc_set_alignment(GTK_MISC(dc->clockw), 0.5, 0.5); // centre text
    gtk_misc_set_padding(GTK_MISC(dc->clockw), 4, 0);        // 4px horizontal padding
    gtk_label_set_justify(GTK_LABEL(dc->clockw), GTK_JUSTIFY_CENTER);
    gtk_container_add(GTK_CONTAINER(dc->main), dc->clockw);
    gtk_widget_show_all(dc->main);
    // Start 1-second recurring timer; handle stored for removal in destructor
    dc->timer = g_timeout_add(1000, (GSourceFunc) clock_update, (gpointer)dc);
    gtk_container_add(GTK_CONTAINER(p->pwid), dc->main);
    RET(1);
}

/*
 * tclock_destructor -- release resources when the plugin is unloaded.
 *
 * Parameters:
 *   p -- plugin_instance pointer.
 *
 * Cleanup:
 *   - Cancels the timer via g_source_remove.
 *   - Destroys dc->main (GTK cascade destroys dc->clockw as well).
 *
 * BUG: dc->calendar_window is not explicitly destroyed here. If the user
 *      unloads the plugin while the calendar is open, the window is orphaned
 *      until the process exits.
 */
static void
tclock_destructor( plugin_instance *p )
{
    tclock_priv *dc = (tclock_priv *) p;

    ENTER;
    // Remove the periodic timer before the widget it references is destroyed
    if (dc->timer)
        g_source_remove(dc->timer);
    gtk_widget_destroy(dc->main);
    // NOTE: dc->calendar_window is NOT closed here if it remains open (bug).
    RET();
}

/* Plugin class descriptor -- located by the plugin loader via class_ptr. */
static plugin_class class = {
    .count       = 0,
    .type        = "tclock",
    .name        = "Text Clock",
    .version     = "2.0",
    .description = "Text clock/date with tooltip",
    .priv_size   = sizeof(tclock_priv),

    .constructor = tclock_constructor,
    .destructor = tclock_destructor,
};
static plugin_class *class_ptr = (plugin_class *) &class;
