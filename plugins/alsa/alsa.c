/*
 * alsa.c -- fbpanel ALSA volume control plugin.
 *
 * Replaces the deprecated OSS volume plugin.  Uses libasound (ALSA) to
 * read and control the Master playback volume on the default sound card.
 *
 * Inherits from the "meter" plugin class (alsa_priv must be the FIRST
 * member of the struct so that an alsa_priv* can be safely cast to
 * meter_priv* or plugin_instance*).
 *
 * Features:
 *   - Polls current volume every 500 ms and updates meter icons + tooltip
 *   - Left-click:   toggle a floating vertical slider popup window
 *   - Middle-click: toggle mute (saves/restores volume)
 *   - Scroll:       adjust volume +-2% (clamped 0..100)
 *   - Auto-hide slider when pointer leaves for >= 1200 ms
 *
 * Configuration (xconf keys):
 *   Card    -- ALSA card identifier (default: "default")
 *   Control -- Mixer element name (default: "Master")
 */

#include "misc.h"
#include "../meter/meter.h"
#include <alsa/asoundlib.h>

//#define DEBUGPRN
#include "dbg.h"

/* Icon name arrays for the meter widget.
 * names[]   - three icons for min/medium/max unmuted volume.
 * s_names[] - single muted icon shown when volume is 0 or muted. */
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
 * alsa_priv -- private per-instance state for the alsa plugin.
 *
 * meter      - embedded parent state (MUST be first; enables plugin_instance* cast)
 * mixer      - open ALSA mixer handle
 * elem       - the selected mixer element (e.g. "Master")
 * vol_min    - ALSA raw volume minimum for elem
 * vol_max    - ALSA raw volume maximum for elem
 * has_switch - TRUE if elem has a playback mute switch
 * vol        - last known volume 0..100; 200 = first-run sentinel
 * muted_vol  - saved volume before muting; restored on unmute
 * muted      - TRUE while in software-muted state
 * update_id  - g_timeout_add source ID for 500 ms poll timer
 * leave_id   - g_timeout_add source ID for 1200 ms auto-hide timer
 * has_pointer- pointer enter/leave reference count for slider window
 * slider_window - floating volume slider popup (NULL when hidden)
 * slider     - the GtkVScale widget inside slider_window
 */
typedef struct {
    meter_priv    meter;          /* parent class -- MUST be first */
    snd_mixer_t  *mixer;
    snd_mixer_elem_t *elem;
    long          vol_min;
    long          vol_max;
    gboolean      has_switch;
    int           vol;
    int           muted_vol;
    gboolean      muted;
    int           update_id;
    int           leave_id;
    int           has_pointer;
    GtkWidget    *slider_window;
    GtkWidget    *slider;
} alsa_priv;

/* Shared pointer to the meter plugin class. */
static meter_class *k;

/* Forward declarations. */
static void   slider_changed(GtkRange *range, alsa_priv *c);
static gboolean crossed(GtkWidget *w, GdkEventCrossing *ev, alsa_priv *c);

/* ---------------------------------------------------------------------------
 * ALSA helpers
 * ------------------------------------------------------------------------- */

/* alsa_get_vol -- read current playback volume, return 0..100. */
static int
alsa_get_vol(alsa_priv *c)
{
    long raw = c->vol_min;
    snd_mixer_handle_events(c->mixer);
    snd_mixer_selem_get_playback_volume(c->elem,
        SND_MIXER_SCHN_FRONT_LEFT, &raw);
    if (c->vol_max <= c->vol_min)
        return 0;
    return (int)((raw - c->vol_min) * 100 / (c->vol_max - c->vol_min));
}

/* alsa_set_vol -- write a 0..100 percent level to the ALSA element. */
static void
alsa_set_vol(alsa_priv *c, int pct)
{
    long raw;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    raw = c->vol_min + (long)pct * (c->vol_max - c->vol_min) / 100;
    snd_mixer_selem_set_playback_volume_all(c->elem, raw);
}

/* alsa_get_hw_mute -- TRUE if the hardware switch says muted (switch=0). */
static gboolean
alsa_get_hw_mute(alsa_priv *c)
{
    int sw = 1;
    if (c->has_switch)
        snd_mixer_selem_get_playback_switch(c->elem,
            SND_MIXER_SCHN_FRONT_LEFT, &sw);
    return (sw == 0);
}

/* alsa_set_hw_mute -- set the hardware mute switch. */
static void
alsa_set_hw_mute(alsa_priv *c, gboolean mute)
{
    if (c->has_switch)
        snd_mixer_selem_set_playback_switch_all(c->elem, mute ? 0 : 1);
}

/* ---------------------------------------------------------------------------
 * GUI update
 * ------------------------------------------------------------------------- */

static gboolean
alsa_update_gui(alsa_priv *c)
{
    int volume;
    gboolean hw_muted;
    gchar buf[24];

    ENTER;
    snd_mixer_handle_events(c->mixer);
    volume   = alsa_get_vol(c);
    hw_muted = alsa_get_hw_mute(c);

    /* Effective mute: hardware switch OR software zero. */
    gboolean effectively_muted = hw_muted || (c->muted && volume == 0);

    /* Switch icon set when mute state changes. */
    if ((effectively_muted || volume == 0) != (c->vol == 0 || c->muted)) {
        if (!effectively_muted && volume > 0)
            k->set_icons(&c->meter, names);
        else
            k->set_icons(&c->meter, s_names);
    }

    c->vol = volume;
    k->set_level(&c->meter, effectively_muted ? 0 : volume);

    g_snprintf(buf, sizeof(buf), "<b>Volume:</b> %d%%", volume);
    if (!c->slider_window)
        gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, buf);
    else {
        g_signal_handlers_block_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
        gtk_range_set_value(GTK_RANGE(c->slider), volume);
        g_signal_handlers_unblock_by_func(G_OBJECT(c->slider),
            G_CALLBACK(slider_changed), c);
    }
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Slider popup
 * ------------------------------------------------------------------------- */

static void
slider_changed(GtkRange *range, alsa_priv *c)
{
    int volume = (int) gtk_range_get_value(range);
    ENTER;
    alsa_set_vol(c, volume);
    alsa_set_hw_mute(c, FALSE);
    c->muted = FALSE;
    alsa_update_gui(c);
    RET();
}

static gboolean
leave_cb(alsa_priv *c)
{
    ENTER;
    c->leave_id = 0;
    c->has_pointer = 0;
    gtk_widget_destroy(c->slider_window);
    c->slider_window = NULL;
    RET(FALSE);
}

static gboolean
crossed(GtkWidget *widget, GdkEventCrossing *event, alsa_priv *c)
{
    ENTER;
    if (event->type == GDK_ENTER_NOTIFY)
        c->has_pointer++;
    else
        c->has_pointer--;

    if (c->has_pointer > 0) {
        if (c->leave_id) {
            g_source_remove(c->leave_id);
            c->leave_id = 0;
        }
    } else {
        if (!c->leave_id && c->slider_window)
            c->leave_id = g_timeout_add(1200, (GSourceFunc) leave_cb, c);
    }
    RET(FALSE);
}

static GtkWidget *
alsa_create_slider(alsa_priv *c)
{
    GtkWidget *win, *frame, *slider;

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 180, 180);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(win), 1);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_MOUSE);
    gtk_window_stick(GTK_WINDOW(win));

    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    gtk_container_add(GTK_CONTAINER(win), frame);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 1);

    slider = gtk_vscale_new_with_range(0.0, 100.0, 1.0);
    gtk_widget_set_size_request(slider, 25, 82);
    gtk_scale_set_draw_value(GTK_SCALE(slider), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(slider), GTK_POS_BOTTOM);
    gtk_scale_set_digits(GTK_SCALE(slider), 0);
    gtk_range_set_inverted(GTK_RANGE(slider), TRUE);
    gtk_range_set_value(GTK_RANGE(slider), ((meter_priv *) c)->level);

    g_signal_connect(G_OBJECT(slider), "value_changed",
        G_CALLBACK(slider_changed), c);
    g_signal_connect(G_OBJECT(slider), "enter-notify-event",
        G_CALLBACK(crossed), c);
    g_signal_connect(G_OBJECT(slider), "leave-notify-event",
        G_CALLBACK(crossed), c);

    gtk_container_add(GTK_CONTAINER(frame), slider);
    c->slider = slider;
    return win;
}

/* ---------------------------------------------------------------------------
 * Event handlers
 * ------------------------------------------------------------------------- */

static gboolean
icon_clicked(GtkWidget *widget, GdkEventButton *event, alsa_priv *c)
{
    ENTER;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        if (!c->slider_window) {
            c->slider_window = alsa_create_slider(c);
            gtk_widget_show_all(c->slider_window);
            gtk_widget_set_tooltip_markup(((plugin_instance *)c)->pwid, NULL);
        } else {
            gtk_widget_destroy(c->slider_window);
            c->slider_window = NULL;
            if (c->leave_id) {
                g_source_remove(c->leave_id);
                c->leave_id = 0;
            }
        }
        RET(FALSE);
    }
    if (!(event->type == GDK_BUTTON_PRESS && event->button == 2))
        RET(FALSE);

    /* Middle-click: toggle mute. */
    if (c->muted) {
        alsa_set_vol(c, c->muted_vol);
        alsa_set_hw_mute(c, FALSE);
        c->muted = FALSE;
    } else {
        c->muted_vol = c->vol;
        alsa_set_hw_mute(c, TRUE);
        c->muted = TRUE;
    }
    alsa_update_gui(c);
    RET(FALSE);
}

static gboolean
icon_scrolled(GtkWidget *widget, GdkEventScroll *event, alsa_priv *c)
{
    int volume;
    ENTER;
    volume = c->muted ? c->muted_vol : c->vol;
    volume += 2 * ((event->direction == GDK_SCROLL_UP ||
                    event->direction == GDK_SCROLL_LEFT) ? 1 : -1);
    if (volume > 100) volume = 100;
    if (volume < 0)   volume = 0;

    if (c->muted) {
        c->muted_vol = volume;
    } else {
        alsa_set_vol(c, volume);
        alsa_update_gui(c);
    }
    RET(TRUE);
}

/* ---------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int
alsa_constructor(plugin_instance *p)
{
    alsa_priv *c;
    snd_mixer_selem_id_t *sid;
    gchar *card = NULL;
    gchar *ctrl = NULL;

    if (!(k = class_get("meter"))) {
        g_message("alsa: 'meter' plugin unavailable — plugin disabled");
        RET(0);
    }
    if (!PLUGIN_CLASS(k)->constructor(p)) {
        g_message("alsa: meter constructor failed — plugin disabled");
        class_put("meter");
        RET(0);
    }
    c = (alsa_priv *) p;

    /* Read optional config keys. */
    XCG(p->xc, "Card",    &card, str);
    XCG(p->xc, "Control", &ctrl, str);
    if (!card || !card[0]) card = "default";
    if (!ctrl || !ctrl[0]) ctrl = "Master";

    /* Open ALSA mixer. */
    if (snd_mixer_open(&c->mixer, 0) < 0) {
        g_message("alsa: snd_mixer_open failed — plugin disabled");
        PLUGIN_CLASS(k)->destructor(p);
        class_put("meter");
        RET(0);
    }
    if (snd_mixer_attach(c->mixer, card) < 0) {
        g_message("alsa: cannot attach to card '%s' — plugin disabled", card);
        snd_mixer_close(c->mixer);
        PLUGIN_CLASS(k)->destructor(p);
        class_put("meter");
        RET(0);
    }
    snd_mixer_selem_register(c->mixer, NULL, NULL);
    snd_mixer_load(c->mixer);

    /* Find the requested element. */
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_name(sid, ctrl);
    snd_mixer_selem_id_set_index(sid, 0);
    c->elem = snd_mixer_find_selem(c->mixer, sid);
    if (!c->elem) {
        g_message("alsa: element '%s' not found on card '%s' — plugin disabled",
                  ctrl, card);
        snd_mixer_close(c->mixer);
        PLUGIN_CLASS(k)->destructor(p);
        class_put("meter");
        RET(0);
    }

    snd_mixer_selem_get_playback_volume_range(c->elem,
        &c->vol_min, &c->vol_max);
    c->has_switch = snd_mixer_selem_has_playback_switch(c->elem);

    k->set_icons(&c->meter, names);

    /* Sentinel so first update always refreshes icon set. */
    c->vol = 200;

    alsa_update_gui(c);
    c->update_id = g_timeout_add(500, (GSourceFunc) alsa_update_gui, c);

    g_signal_connect(G_OBJECT(p->pwid), "scroll-event",
        G_CALLBACK(icon_scrolled), c);
    g_signal_connect(G_OBJECT(p->pwid), "button_press_event",
        G_CALLBACK(icon_clicked), c);
    g_signal_connect(G_OBJECT(p->pwid), "enter-notify-event",
        G_CALLBACK(crossed), c);
    g_signal_connect(G_OBJECT(p->pwid), "leave-notify-event",
        G_CALLBACK(crossed), c);

    RET(1);
}

static void
alsa_destructor(plugin_instance *p)
{
    alsa_priv *c = (alsa_priv *) p;
    ENTER;
    g_source_remove(c->update_id);
    if (c->slider_window)
        gtk_widget_destroy(c->slider_window);
    if (c->leave_id)
        g_source_remove(c->leave_id);
    snd_mixer_close(c->mixer);
    PLUGIN_CLASS(k)->destructor(p);
    class_put("meter");
    RET();
}

static plugin_class class = {
    .count       = 0,
    .type        = "alsa",
    .name        = "ALSA Volume",
    .version     = "1.0",
    .description = "ALSA volume control (replaces OSS volume plugin)",
    .priv_size   = sizeof(alsa_priv),
    .constructor = alsa_constructor,
    .destructor  = alsa_destructor,
};

static plugin_class *class_ptr = (plugin_class *) &class;
