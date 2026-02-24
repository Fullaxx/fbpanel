/*
 * timer.c -- fbpanel countdown timer plugin.
 *
 * A configurable countdown timer displayed as a label.  Three states:
 *   Idle    -- shows the configured duration ("5:00").
 *   Running -- ticks down every second ("4:59", "4:58", ...).
 *   Alarmed -- shows "DONE" and flashes until clicked.
 *
 * Left-click cycles: Idle -> Running -> Idle (reset).
 * When alarmed, any click resets to Idle.
 *
 * No new library dependencies -- pure GLib/GTK2.
 *
 * Configuration (xconf keys):
 *   Duration -- countdown duration in seconds (default: 300 = 5 minutes).
 *   Label    -- prefix shown in idle state (default: none, just the time).
 */

#include <stdio.h>
#include <string.h>

#include "panel.h"
#include "misc.h"
#include "plugin.h"

//#define DEBUGPRN
#include "dbg.h"

typedef enum {
    TIMER_IDLE    = 0,
    TIMER_RUNNING = 1,
    TIMER_ALARMED = 2
} timer_state_t;

typedef struct {
    plugin_instance  plugin;
    GtkWidget       *label;
    guint            tick_id;       /* g_timeout_add source ID, 0 = inactive */
    timer_state_t    state;
    int              duration;      /* configured duration in seconds */
    int              remaining;     /* seconds remaining while running */
    gboolean         flash_on;      /* toggle for alarm flash */
    guint            flash_id;      /* g_timeout_add source ID for flashing */
} timer_priv;

/* ---------------------------------------------------------------------------
 * Display helpers
 * ------------------------------------------------------------------------- */

static void
timer_set_label(timer_priv *priv)
{
    gchar text[32];
    int sec, min;

    switch (priv->state) {
    case TIMER_IDLE:
        min = priv->duration / 60;
        sec = priv->duration % 60;
        g_snprintf(text, sizeof(text), "%d:%02d", min, sec);
        gtk_label_set_markup(GTK_LABEL(priv->label), text);
        break;

    case TIMER_RUNNING:
        min = priv->remaining / 60;
        sec = priv->remaining % 60;
        g_snprintf(text, sizeof(text), "%d:%02d", min, sec);
        gtk_label_set_markup(GTK_LABEL(priv->label), text);
        break;

    case TIMER_ALARMED:
        gtk_label_set_markup(GTK_LABEL(priv->label),
            priv->flash_on ? "<b>DONE</b>" : "DONE");
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Timer callbacks
 * ------------------------------------------------------------------------- */

/* Forward declaration -- timer_flash is defined after timer_tick but called
 * from within it when the countdown reaches zero. */
static gboolean timer_flash(timer_priv *priv);

/* Countdown tick -- called every second while running. */
static gboolean
timer_tick(timer_priv *priv)
{
    ENTER;
    if (priv->state != TIMER_RUNNING) {
        priv->tick_id = 0;
        RET(FALSE);
    }

    priv->remaining--;
    if (priv->remaining <= 0) {
        priv->remaining = 0;
        priv->state     = TIMER_ALARMED;
        priv->flash_on  = TRUE;
        timer_set_label(priv);
        priv->tick_id = 0;
        /* Start flashing at 500 ms intervals. */
        priv->flash_id = g_timeout_add(500, (GSourceFunc) timer_flash, priv);
        RET(FALSE);
    }

    timer_set_label(priv);
    RET(TRUE);
}

/* Flash callback -- called every 500ms while alarmed.
 * Reuses the same function: in ALARMED state it toggles flash_on. */
static gboolean
timer_flash(timer_priv *priv)
{
    ENTER;
    if (priv->state != TIMER_ALARMED) {
        priv->flash_id = 0;
        RET(FALSE);
    }
    priv->flash_on = !priv->flash_on;
    timer_set_label(priv);
    RET(TRUE);
}

static void
timer_reset(timer_priv *priv)
{
    if (priv->tick_id) {
        g_source_remove(priv->tick_id);
        priv->tick_id = 0;
    }
    if (priv->flash_id) {
        g_source_remove(priv->flash_id);
        priv->flash_id = 0;
    }
    priv->state     = TIMER_IDLE;
    priv->remaining = priv->duration;
    priv->flash_on  = FALSE;
    timer_set_label(priv);
}

/* ---------------------------------------------------------------------------
 * Event handler
 * ------------------------------------------------------------------------- */

static gboolean
timer_clicked(GtkWidget *widget, GdkEventButton *event, timer_priv *priv)
{
    ENTER;
    if (event->type != GDK_BUTTON_PRESS || event->button != 1)
        RET(FALSE);

    switch (priv->state) {
    case TIMER_IDLE:
        priv->state     = TIMER_RUNNING;
        priv->remaining = priv->duration;
        timer_set_label(priv);
        priv->tick_id = g_timeout_add(1000, (GSourceFunc) timer_tick, priv);
        break;

    case TIMER_RUNNING:
    case TIMER_ALARMED:
        timer_reset(priv);
        break;
    }
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
timer_constructor(plugin_instance *p)
{
    timer_priv *priv;

    ENTER;
    priv = (timer_priv *) p;
    priv->duration  = 300;
    priv->state     = TIMER_IDLE;

    XCG(p->xc, "Duration", &priv->duration, int);
    if (priv->duration < 1)   priv->duration = 1;
    if (priv->duration > 86400) priv->duration = 86400;
    priv->remaining = priv->duration;

    priv->label = gtk_label_new("...");
    gtk_container_add(GTK_CONTAINER(p->pwid), priv->label);
    gtk_widget_show(priv->label);
    gtk_widget_add_events(p->pwid, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(p->pwid), "button-press-event",
                     G_CALLBACK(timer_clicked), priv);

    gtk_widget_set_tooltip_markup(p->pwid,
        "<b>Countdown Timer</b>\n"
        "Left-click to start; click again to reset.");

    timer_set_label(priv);
    RET(1);
}

static void
timer_destructor(plugin_instance *p)
{
    timer_priv *priv = (timer_priv *) p;
    ENTER;
    if (priv->tick_id) {
        g_source_remove(priv->tick_id);
        priv->tick_id = 0;
    }
    if (priv->flash_id) {
        g_source_remove(priv->flash_id);
        priv->flash_id = 0;
    }
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "timer",
    .name        = "Timer",
    .version     = "1.0",
    .description = "Configurable countdown timer",
    .priv_size   = sizeof(timer_priv),
    .constructor = timer_constructor,
    .destructor  = timer_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
