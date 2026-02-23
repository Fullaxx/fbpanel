/*
 * OSS volume plugin. Will works with ALSA since it usually
 * emulates OSS layer.
 *
 * volume.c -- OSS mixer volume control plugin for fbpanel.
 *
 * Inherits from the "meter" plugin class (meter_priv must be the FIRST
 * member of volume_priv so that a volume_priv* can be safely cast to
 * meter_priv* or plugin_instance*).
 *
 * Features:
 *   - Reads/writes OSS mixer via /dev/mixer ioctl (SOUND_MIXER_VOLUME channel)
 *   - Polls current volume every 1000 ms and updates meter icons + tooltip
 *   - Left-click:   toggle a floating vertical slider popup window
 *   - Middle-click: toggle mute (saves/restores volume)
 *   - Scroll:       adjust volume ±2% (clamped 0..100)
 *   - Auto-hide slider when pointer leaves for ≥1200 ms
 *
 * Fixed (BUG-011): volume_destructor now calls close(c->fd) to release
 *      the /dev/mixer file descriptor when the plugin is unloaded.
 */

#include "misc.h"
#include "../meter/meter.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if defined __linux__
#include <linux/soundcard.h>
#endif

//#define DEBUGPRN
#include "dbg.h"

/*
 * Icon name arrays for the meter widget.
 *
 * names[]   - three icons representing min/medium/max unmuted volume.
 *             The meter plugin selects among them based on the level.
 * s_names[] - single "muted" icon shown when volume is 0.
 */
static gchar *names[] = {
    "stock_volume-min",
    "stock_volume-med",
    "stock_volume-max",
    NULL
};

static gchar *s_names[] = {
    "stock_volume-mute",
    NULL
};

/*
 * volume_priv -- private per-instance state for the volume plugin.
 *
 * meter  - embedded parent state (MUST be first; enables plugin_instance* cast)
 * fd     - open file descriptor for /dev/mixer (O_RDWR)
 * chan   - OSS mixer channel index (SOUND_MIXER_VOLUME)
 * vol    - last known volume level 0..100; initialised to 200 as a sentinel
 *          so the first volume_update_gui() always refreshes the icon set
 * muted_vol - volume saved before muting; restored on unmute
 * update_id - g_timeout_add() source id for the 1000 ms poll timer
 * leave_id  - g_timeout_add() source id for the 1200 ms auto-hide timer
 * has_pointer - reference count of pointer enter events; > 0 means the
 *               pointer is inside the slider window
 * muted       - TRUE while the plugin is in muted state
 * slider_window - the floating volume-slider popup (NULL when hidden)
 * slider        - the GtkVScale widget inside slider_window
 */
typedef struct {
    meter_priv meter;       /* parent class state -- MUST be first member */
    int fd, chan;
    guchar vol, muted_vol;
    int update_id, leave_id;
    int has_pointer;
    gboolean muted;
    GtkWidget *slider_window;
    GtkWidget *slider;
} volume_priv;

/* Shared pointer to the meter plugin class (loaded via class_get("meter")).
 * Used to call parent constructor/destructor and meter helper methods.    */
static meter_class *k;

/* Forward declarations needed by volume_create_slider.                    */
static void slider_changed(GtkRange *range, volume_priv *c);
static gboolean crossed(GtkWidget *widget, GdkEventCrossing *event,
    volume_priv *c);

/*
 * oss_get_volume -- read the current OSS mixer volume for channel c->chan.
 *
 * MIXER_READ ioctl fills `volume` with a packed stereo value:
 *   high byte = right channel, low byte = left channel (0..100 each).
 * We mask to 0xFF to return just the left-channel level.
 *
 * Returns 0 on ioctl error.
 */
static int
oss_get_volume(volume_priv *c)
{
    int volume;

    ENTER;
    if (ioctl(c->fd, MIXER_READ(c->chan), &volume)) {
        ERR("volume: can't get volume from /dev/mixer\n");
        RET(0);
    }
    /* OSS returns a stereo pair; keep only the left-channel byte.         */
    volume &= 0xFF;
    DBG("volume=%d\n", volume);
    RET(volume);
}

/*
 * oss_set_volume -- write a new volume level to the OSS mixer.
 *
 * The OSS MIXER_WRITE ioctl expects a packed stereo value.
 * `(volume << 8) | volume` replicates the 0..100 value in both the
 * high byte (right channel) and the low byte (left channel), setting
 * both channels to the same level.
 */
static void
oss_set_volume(volume_priv *c, int volume)
{
    ENTER;
    DBG("volume=%d\n", volume);
    /* Pack into OSS stereo format: high byte = right, low byte = left.   */
    volume = (volume << 8) | volume;
    ioctl(c->fd, MIXER_WRITE(c->chan), &volume);
    RET();
}

/*
 * volume_update_gui -- poll the mixer and refresh the meter widget.
 *
 * Called every 1000 ms by the update_id timer, and immediately after any
 * slider or mute state change.
 *
 * Behaviour:
 *  1. Read current OSS volume.
 *  2. If mute state changed (0 ↔ nonzero), swap the icon set.
 *  3. Update the meter level bar.
 *  4. Format a "<b>Volume:</b> N%" tooltip string.
 *     - If slider is hidden: set as tooltip markup on the panel button.
 *     - If slider is visible: update the slider position instead
 *       (blocking slider_changed to prevent a feedback loop).
 *
 * Returns TRUE so g_timeout_add keeps the timer running.
 */
static gboolean
volume_update_gui(volume_priv *c)
{
    int volume;
    gchar buf[20];

    ENTER;
    volume = oss_get_volume(c);

    /* Switch icon set when transitioning between muted (volume==0) and
     * unmuted (volume>0).  Uses c->vol as the previous value.             */
    if ((volume != 0) != (c->vol != 0)) {
        if (volume)
            k->set_icons(&c->meter, names);   /* unmuted icon set          */
        else
            k->set_icons(&c->meter, s_names); /* muted icon                */
        DBG("seting %s icons\n", volume ? "normal" : "muted");
    }

    c->vol = volume;
    k->set_level(&c->meter, volume);   /* update the meter bar            */

    g_snprintf(buf, sizeof(buf), "<b>Volume:</b> %d%%", volume);
    if (!c->slider_window)
        /* Slider hidden: show volume as a tooltip on the panel button.    */
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    else {
        /* Slider visible: update slider position.  Block slider_changed
         * while doing so to avoid a set→changed→set→... feedback loop.   */
        g_signal_handlers_block_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
        gtk_range_set_value(GTK_RANGE(c->slider), volume);
        g_signal_handlers_unblock_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
    }
    RET(TRUE);
}

/*
 * slider_changed -- callback for "value_changed" on the slider GtkVScale.
 *
 * The user dragged the slider to a new position.  Write the new volume
 * to the OSS mixer and refresh the meter widget.
 */
static void
slider_changed(GtkRange *range, volume_priv *c)
{
    int volume = (int) gtk_range_get_value(range);
    ENTER;
    DBG("value=%d\n", volume);
    oss_set_volume(c, volume);
    volume_update_gui(c);
    RET();
}

/*
 * volume_create_slider -- build and return the floating slider popup window.
 *
 * Creates a GTK_WINDOW_TOPLEVEL popup (undecorated, not in taskbar/pager,
 * positioned at the mouse cursor) containing an etched frame with a
 * 0..100 vertical scale (inverted so up = louder).
 *
 * The slider window is assigned to c->slider_window by icon_clicked; this
 * function only returns the GtkWindow widget.
 *
 * Enter/leave events on the slider are connected to crossed() for the
 * auto-hide timeout.
 */
static GtkWidget *
volume_create_slider(volume_priv *c)
{
    GtkWidget *slider, *win;
    GtkWidget *frame;

    /* Floating, undecorated, non-resizable popup.                         */
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 1);
    /* Keep the slider out of the taskbar and pager.                       */
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    /* Place popup near the mouse cursor.                                  */
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    /* Appear on all virtual desktops.                                     */
    gtk_window_stick(GTK_WINDOW(win));

    /* Etched frame provides a visual border around the slider.            */
    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_add(GTK_CONTAINER(win), frame);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 1);

    /* Vertical scale: 0 (bottom) to 100 (top), step 1.
     * Inverted so dragging up increases the value.
     * Initial position comes from the meter's current level.             */
    slider = gtk_vscale_new_with_range(0.0, 100.0, 1.0);
    gtk_widget_set_size_request(slider, 25, 82);
    gtk_scale_set_draw_value(GTK_SCALE(slider), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(slider), GTK_POS_BOTTOM);
    gtk_scale_set_digits(GTK_SCALE(slider), 0);
    gtk_range_set_inverted(GTK_RANGE(slider), TRUE);  /* up = louder        */
    gtk_range_set_value(GTK_RANGE(slider), ((meter_priv *) c)->level);
    DBG("meter->level %f\n", ((meter_priv *) c)->level);

    /* When the user moves the slider, apply the new volume.               */
    g_signal_connect(G_OBJECT(slider), "value_changed",
        G_CALLBACK(slider_changed), c);
    /* Track pointer enter/leave on the slider for auto-hide logic.        */
    g_signal_connect(G_OBJECT(slider), "enter-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    g_signal_connect(G_OBJECT(slider), "leave-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    gtk_container_add(GTK_CONTAINER(frame), slider);

    c->slider = slider;   /* store reference for signal blocking in update */
    return win;
}

/*
 * icon_clicked -- handle button-press on the panel meter widget.
 *
 * Left-click (button 1):
 *   - If slider is hidden: create and show the floating slider popup.
 *     Clear the tooltip markup (replacing it with NULL) while slider is up.
 *   - If slider is visible: destroy the slider popup and cancel any pending
 *     leave timer.
 *
 * Middle-click (button 2):
 *   - Toggle mute state.
 *   - When muting:   save current vol to muted_vol, set volume to 0.
 *   - When unmuting: restore volume from muted_vol.
 *
 * Other buttons: fall through and return FALSE (unhandled).
 */
static gboolean
icon_clicked(GtkWidget *widget, GdkEventButton *event, volume_priv *c)
{
    int volume;

    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        if (c->slider_window == NULL) {
            /* Open the slider popup.                                      */
            c->slider_window = volume_create_slider(c);
            gtk_widget_show_all(c->slider_window);
            /* Clear tooltip while slider is visible.                      */
            gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, NULL);
        } else {
            /* Close the slider popup and cancel the leave timer.          */
            gtk_widget_destroy(c->slider_window);
            c->slider_window = NULL;
            if (c->leave_id) {
                g_source_remove(c->leave_id);
                c->leave_id = 0;
            }
        }
        RET(FALSE);
    }
    /* Middle-click: toggle mute.                                          */
    if (!(event->type == GDK_BUTTON_PRESS && event->button == 2))
        RET(FALSE);

    if (c->muted) {
        /* Unmute: restore saved volume.                                   */
        volume = c->muted_vol;
    } else {
        /* Mute: save current volume and set to 0.                        */
        c->muted_vol = c->vol;
        volume = 0;
    }
    c->muted = !c->muted;
    oss_set_volume(c, volume);
    volume_update_gui(c);
    RET(FALSE);
}

/*
 * icon_scrolled -- handle scroll-wheel events on the panel meter widget.
 *
 * Scroll up or left:  +2% volume
 * Scroll down or right: -2% volume
 * Volume is clamped to 0..100.
 *
 * If currently muted, the adjustment is applied to muted_vol (the saved
 * pre-mute volume) without actually calling oss_set_volume, so that when
 * the user unmutes the new value takes effect.
 */
static gboolean
icon_scrolled(GtkWidget *widget, GdkEventScroll *event, volume_priv *c)
{
    int volume;

    ENTER;
    /* Use muted_vol when muted so scroll accumulates without un-muting.   */
    volume = (c->muted) ? c->muted_vol : ((meter_priv *) c)->level;
    volume += 2 * ((event->direction == GDK_SCROLL_UP
            || event->direction == GDK_SCROLL_LEFT) ? 1 : -1);
    /* Clamp to valid OSS range.                                           */
    if (volume > 100)
        volume = 100;
    if (volume < 0)
        volume = 0;

    if (c->muted)
        /* Muted: update only the saved pre-mute volume.                  */
        c->muted_vol = volume;
    else {
        oss_set_volume(c, volume);
        volume_update_gui(c);
    }
    RET(TRUE);
}

/*
 * leave_cb -- auto-hide timer callback fired 1200 ms after pointer leave.
 *
 * Destroys the slider popup.  Installed by crossed() when has_pointer
 * drops to zero.  Cleared by crossed() if the pointer re-enters before
 * it fires.
 *
 * Returns FALSE so g_timeout_add does not repeat the callback.
 */
static gboolean
leave_cb(volume_priv *c)
{
    ENTER;
    c->leave_id = 0;
    c->has_pointer = 0;
    gtk_widget_destroy(c->slider_window);
    c->slider_window = NULL;
    RET(FALSE);
}

/*
 * crossed -- handle pointer enter/leave events on the slider popup.
 *
 * Both the slider widget and the meter panel button connect enter and
 * leave events to this handler (also called by volume_constructor for
 * the main panel widget events).
 *
 * Enter: increment has_pointer; cancel any pending leave timer.
 * Leave: decrement has_pointer; if it drops to zero, schedule a 1200 ms
 *        auto-hide timer (leave_cb).
 *
 * Using a counter rather than a boolean handles the case where the pointer
 * crosses between overlapping widgets (slider frame → slider scale) without
 * the window disappearing.
 */
static gboolean
crossed(GtkWidget *widget, GdkEventCrossing *event, volume_priv *c)
{
    ENTER;
    if (event->type == GDK_ENTER_NOTIFY)
        c->has_pointer++;
    else
        c->has_pointer--;

    if (c->has_pointer > 0) {
        /* Pointer is still inside: cancel any scheduled hide timer.       */
        if (c->leave_id) {
            g_source_remove(c->leave_id);
            c->leave_id = 0;
        }
    } else {
        /* Pointer has left: schedule auto-hide after 1200 ms.             */
        if (!c->leave_id && c->slider_window) {
            c->leave_id = g_timeout_add(1200, (GSourceFunc) leave_cb, c);
        }
    }
    DBG("has_pointer=%d\n", c->has_pointer);
    RET(FALSE);
}

/*
 * volume_constructor -- allocate and initialise the volume plugin instance.
 *
 * Steps:
 *  1. Load the "meter" parent class and call its constructor (which creates
 *     the meter widget and adds it to p->pwid).
 *  2. Open /dev/mixer O_RDWR; fail if unavailable.
 *  3. Set the initial (unmuted) icon set.
 *  4. Start the 1000 ms update timer.
 *  5. Set c->vol = 200 as a sentinel so the first update_gui() always
 *     triggers an icon-set switch (the real volume is 0..100).
 *  6. Set the OSS channel to SOUND_MIXER_VOLUME.
 *  7. Read and display the current volume.
 *  8. Connect scroll, button-press, enter, and leave events on p->pwid.
 *
 * Returns 1 on success, 0 on failure.
 */
static int
volume_constructor(plugin_instance *p)
{
    volume_priv *c;

    if (!(k = class_get("meter"))) {
        g_message("volume: 'meter' plugin unavailable — plugin disabled");
        RET(0);
    }
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("volume: meter constructor failed — plugin disabled");
        RET(0);
    }
    c = (volume_priv *) p;

    /* Open the OSS mixer device.  Soft-fail if not available (e.g. on
     * systems without OSS/ALSA, inside containers, or VMs with no audio).
     * The panel continues running; only this plugin is skipped.
     *
     * IMPORTANT: meter_constructor() has already run and connected update_view
     * to the global icon_theme "changed" signal (with `m` as the swapped user
     * data).  If we return 0 here without calling meter_destructor(), that
     * signal handler remains connected after plugin_put() frees the struct,
     * causing a use-after-free when the icon theme is next scanned (BUG-016). */
    if ((c->fd = open ("/dev/mixer", O_RDWR, 0)) < 0) {
        g_message("volume: /dev/mixer not available — plugin disabled");
        PLUGIN_CLASS(k)->destructor(p); /* disconnect icon_theme signal       */
        class_put("meter");             /* balance class_get() above           */
        RET(0);
    }

    k->set_icons(&c->meter, names);   /* start with unmuted icon set       */

    /* Poll the mixer every 1000 ms.                                       */
    c->update_id = g_timeout_add(1000, (GSourceFunc) volume_update_gui, c);

    /* Sentinel value: differs from all valid 0..100 OSS levels so the
     * first volume_update_gui() always updates the icon set.              */
    c->vol = 200;

    c->chan = SOUND_MIXER_VOLUME;   /* OSS master volume channel           */
    volume_update_gui(c);           /* initial GUI sync                    */

    /* Scroll events change volume by ±2%.                                 */
    g_signal_connect(G_OBJECT(p->pwid), "scroll-event",
        G_CALLBACK(icon_scrolled), (gpointer) c);
    /* Left/middle button clicks on the meter button.                      */
    g_signal_connect(G_OBJECT(p->pwid), "button_press_event",
        G_CALLBACK(icon_clicked), (gpointer)c);
    /* Enter/leave events on the panel button for auto-hide hysteresis.    */
    g_signal_connect(G_OBJECT(p->pwid), "enter-notify-event",
        G_CALLBACK(crossed), (gpointer)c);
    g_signal_connect(G_OBJECT(p->pwid), "leave-notify-event",
        G_CALLBACK(crossed), (gpointer)c);

    RET(1);
}

/*
 * volume_destructor -- clean up the volume plugin instance.
 *
 * Stops the update timer, destroys the slider popup if open, calls the
 * parent meter destructor, and releases the meter class reference.
 *
 */
static void
volume_destructor(plugin_instance *p)
{
    volume_priv *c = (volume_priv *) p;

    ENTER;
    g_source_remove(c->update_id);           /* stop the 1000 ms timer    */
    if (c->slider_window)
        gtk_widget_destroy(c->slider_window);/* close slider popup if open */
    close(c->fd);                            /* release /dev/mixer fd      */
    PLUGIN_CLASS(k)->destructor(p);          /* parent class cleanup       */
    class_put("meter");                      /* decrement class refcount   */
    RET();
}


/*
 * plugin_class descriptor -- exported symbol used by the plugin loader.
 *
 * priv_size = sizeof(volume_priv) causes the loader to allocate enough
 * space for the full private struct (meter_priv + volume fields).
 */
static plugin_class class = {
    .count       = 0,
    .type        = "volume",
    .name        = "Volume",
    .version     = "2.0",
    .description = "OSS volume control",
    .priv_size   = sizeof(volume_priv),
    .constructor = volume_constructor,
    .destructor  = volume_destructor,
};

/* class_ptr is the symbol the plugin loader looks up via dlsym().        */
static plugin_class *class_ptr = (plugin_class *) &class;
