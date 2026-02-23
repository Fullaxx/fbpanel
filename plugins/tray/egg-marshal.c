/*
 * egg-marshal.c -- compilation unit for custom GObject signal marshalers.
 *
 * Includes the header (which declares the marshal functions) and the
 * generated implementation (eggmarshalers.c.inc, produced by
 * glib-genmarshal from eggmarshalers.list).
 *
 * The three custom marshalers declared here are:
 *   _egg_marshal_VOID__OBJECT_OBJECT       (GObject*, GObject*)
 *   _egg_marshal_VOID__OBJECT_STRING_LONG_LONG (GObject*, gchar*, glong, glong)
 *   _egg_marshal_VOID__OBJECT_LONG         (GObject*, glong)
 *
 * These are used by EggTrayManager to emit signals with argument types
 * that are not covered by the built-in GLib marshalers (g_cclosure_marshal_*).
 */
#include "eggmarshalers.h"
#include "eggmarshalers.c.inc"
