/*
 * xconf.h -- fbpanel configuration tree API.
 *
 * fbpanel uses a simple text-based configuration format consisting of
 * whitespace-separated key=value pairs nested inside named blocks:
 *
 *   Global {
 *       edge = bottom
 *       height = 28
 *   }
 *   Plugin {
 *       type = dclock
 *       Config {
 *           ClockFmt = %R
 *       }
 *   }
 *
 * This module parses that format into a tree of xconf nodes and provides
 * typed accessor macros (XCG / XCS) for reading and writing values.
 *
 * Tree structure:
 *   Each node has a name, an optional string value, a list of child nodes
 *   (sons), and a back-pointer to its parent.  Block nodes (e.g., "Global",
 *   "Plugin") have children but no direct value.  Leaf nodes (e.g.,
 *   "edge=bottom") have a value but typically no children.
 *
 * Ownership:
 *   - xconf nodes are heap-allocated; free with xconf_del().
 *   - name and value strings are owned by the node (g_strdup'd on set;
 *     g_free'd on del).
 *   - Sons are owned by their parent; xconf_del(parent, FALSE) recursively
 *     frees all descendants.
 *   - Plugins receive a pointer into the panel's config tree (p->xc).
 *     They must NOT free or modify p->xc — it is owned by the panel.
 */
#ifndef _XCONF_H_
#define _XCONF_H_

#include <glib.h>
#include <stdio.h>

/*
 * xconf -- one node in the configuration tree.
 *
 * Fields:
 *   name   - node name (e.g., "edge", "Plugin", "Config"); g_strdup'd.
 *   value  - node value string (e.g., "bottom", "dclock"); g_strdup'd.
 *            NULL for block nodes that contain only children.
 *   sons   - GSList of child xconf* pointers (in config-file order).
 *            Empty list for leaf nodes.
 *   parent - back-pointer to parent node; NULL for the root node.
 */
typedef struct _xconf
{
    gchar *name;           /* node name; g_malloc'd; freed by xconf_del */
    gchar *value;          /* node value; g_malloc'd; may be NULL */
    GSList *sons;          /* child nodes (GSList of xconf*) */
    struct _xconf *parent; /* parent node; NULL at root */
} xconf;

/*
 * xconf_enum -- string-to-integer mapping for enum-typed config values.
 *
 * Used with xconf_get_enum() / xconf_set_enum() to convert between the
 * string stored in the config file and an integer constant.
 *
 * Fields:
 *   str  - the config-file string (e.g., "top", "bottom", "left", "right")
 *   desc - human-readable description for display in the preferences UI
 *   num  - the corresponding integer constant (e.g., EDGE_TOP, EDGE_BOTTOM)
 *
 * The last entry in an xconf_enum array must have str == NULL.
 */
typedef struct {
    gchar *str;   /* config-file token */
    gchar *desc;  /* UI label */
    int num;      /* integer constant */
} xconf_enum;

/* --- Tree construction and destruction --- */

/*
 * xconf_new -- allocate a new xconf node.
 *
 * Parameters:
 *   name  - node name; g_strdup'd into the node.
 *   value - node value; g_strdup'd, or NULL for a block node.
 *
 * Returns: a new xconf* with no parent and an empty sons list.
 *          Caller must free with xconf_del(node, FALSE).
 */
xconf *xconf_new(gchar *name, gchar *value);

/*
 * xconf_append -- add son as the last child of parent.
 *
 * Sets son->parent = parent.  Ownership of son transfers to parent;
 * do not free son separately after this call.
 */
void xconf_append(xconf *parent, xconf *son);

/*
 * xconf_append_sons -- move all children of son into parent.
 *
 * Used when merging subtrees; after the call, son has no children.
 */
void xconf_append_sons(xconf *parent, xconf *son);

/*
 * xconf_unlink -- detach a node from its parent without freeing it.
 *
 * After this call, x->parent == NULL and x is no longer in the parent's
 * sons list.  Caller now owns x.
 */
void xconf_unlink(xconf *x);

/*
 * xconf_del -- free an xconf node and optionally all descendants.
 *
 * Parameters:
 *   x         - the node to free.  Must be unlinked from its parent first
 *               (or be the root).
 *   sons_only - if TRUE, free only the children (leaving x itself allocated);
 *               if FALSE, free x and all descendants recursively.
 *
 * Memory: frees name, value, and (if sons_only==FALSE) the node struct.
 */
void xconf_del(xconf *x, gboolean sons_only);

/* --- Value access --- */

/*
 * xconf_set_value -- set a node's value string (makes a copy).
 *
 * Frees the old value (if any) and g_strdup's the new one.
 * The node takes ownership of the copy; caller keeps its original.
 */
void xconf_set_value(xconf *x, gchar *value);

/*
 * xconf_set_value_ref -- set a node's value taking ownership directly.
 *
 * Like xconf_set_value but does NOT copy: x->value = value.
 * The string must have been g_malloc'd; the node will g_free it.
 */
void xconf_set_value_ref(xconf *x, gchar *value);

/*
 * xconf_get_value -- return the node's raw string value.
 *
 * Returns: x->value (non-owning); NULL if the node has no value.
 *          Do NOT free the returned pointer.
 */
gchar *xconf_get_value(xconf *x);

/* --- File I/O --- */

/*
 * xconf_prn -- serialise a node (and optionally its subtree) to a file.
 *
 * Parameters:
 *   fp        - open FILE* to write to.
 *   x         - root of the subtree to serialise.
 *   n         - indentation level (0 = top-level, increases with nesting).
 *   sons_only - if TRUE, serialise only children (not x's own name/value).
 */
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only);

/*
 * xconf_new_from_file -- parse a config file into an xconf tree.
 *
 * Parameters:
 *   fname - path to the config file.
 *   name  - name to assign to the root node of the returned tree.
 *
 * Returns: the root xconf* of the parsed tree, or NULL on read error.
 *          Caller owns the returned tree; free with xconf_del(root, FALSE).
 */
xconf *xconf_new_from_file(gchar *fname, gchar *name);

/*
 * xconf_save_to_file -- serialise a tree to a named file (atomic write).
 *
 * Parameters:
 *   fname - destination file path.
 *   xc    - root of the tree to serialise.
 */
void xconf_save_to_file(gchar *fname, xconf *xc);

/*
 * xconf_save_to_profile -- save the global panel config to the active profile.
 *
 * Uses the_panel->profile_name to determine the file path.
 */
void xconf_save_to_profile(xconf *xc);

/* --- Node lookup --- */

/*
 * xconf_find -- find the Nth child node with a given name.
 *
 * Parameters:
 *   x    - parent node to search within (immediate children only).
 *   name - name to match (case-sensitive).
 *   no   - 0-based index among matching nodes (0 = first match).
 *
 * Returns: the matching xconf* or NULL if not found.
 *          The returned pointer is non-owning (do not free).
 */
xconf *xconf_find(xconf *x, gchar *name, int no);

/*
 * xconf_dup -- deep-copy a subtree.
 *
 * Returns: a newly allocated xconf* tree that is a complete copy of xc.
 *          Caller owns the copy; free with xconf_del(copy, FALSE).
 */
xconf *xconf_dup(xconf *xc);

/*
 * xconf_cmp -- compare two xconf trees for structural and value equality.
 *
 * Returns: TRUE if a and b have equal names, values, and children recursively.
 */
gboolean xconf_cmp(xconf *a, xconf *b);

/* --- Typed accessors (lower-level, prefer XCG/XCS macros) --- */

/*
 * xconf_get -- find or create a named child node.
 *
 * If a child with 'name' exists, returns it.
 * If not, creates a new child node with that name and no value.
 * Used by XCS() to get-or-create the target node before setting a value.
 */
xconf *xconf_get(xconf *x, gchar *name);

/*
 * xconf_get_int -- read the node's value as an integer (atoi).
 *
 * Parameters:
 *   x   - may be NULL (silently skips if node not found via xconf_find).
 *   val - receives the parsed integer.  Unchanged if x is NULL.
 */
void xconf_get_int(xconf *x, int *val);

/*
 * xconf_get_enum -- read the node's value and map it to an int via a table.
 *
 * Parameters:
 *   x   - may be NULL.
 *   val - receives the matched integer from e[i].num.  Unchanged if no match.
 *   e   - NULL-terminated xconf_enum table.
 */
void xconf_get_enum(xconf *x, int *val, xconf_enum *e);

/*
 * xconf_get_str -- read the node's value as a non-owning string pointer.
 *
 * Parameters:
 *   x   - may be NULL.
 *   val - set to x->value (not a copy!).  The pointed-to string is owned
 *         by the xconf node.  Do NOT g_free(*val).
 */
void xconf_get_str(xconf *x, gchar **val);

/*
 * xconf_get_strdup -- read the node's value as a newly allocated copy.
 *
 * Parameters:
 *   x   - may be NULL.
 *   val - set to g_strdup(x->value).  Caller must g_free(*val).
 */
void xconf_get_strdup(xconf *x, gchar **val);

/* Setter functions: */
void xconf_set_int(xconf *x, int val);                          /* write int as string */
void xconf_set_enum(xconf *x, int val, xconf_enum *e);          /* write enum as string */

/*
 * XCG(xc, name, var, type, extra...) -- type-safe config READ macro.
 *
 * Expands to: xconf_get_<type>(xconf_find(xc, name, 0), var, ##extra)
 *
 * Examples:
 *   int width = 100;
 *   XCG(p->xc, "width", &width, int);
 *
 *   gchar *fmt = NULL;
 *   XCG(p->xc, "ClockFmt", &fmt, str);    // fmt = non-owning ptr into xc
 *
 *   int edge = EDGE_BOTTOM;
 *   XCG(p->xc, "edge", &edge, enum, edge_enum);
 *
 * If the key is not found, xconf_find returns NULL and xconf_get_* silently
 * leaves *var unchanged — so defaults set before XCG() are preserved.
 */
#define XCG(xc, name, var, type, extra...)                      \
    xconf_get_ ## type(xconf_find(xc, name, 0), var, ## extra)

/*
 * XCS(xc, name, var, type, extra...) -- type-safe config WRITE macro.
 *
 * Expands to: xconf_set_<type>(xconf_get(xc, name), var, ##extra)
 * xconf_get() creates the node if it doesn't exist.
 *
 * Examples:
 *   XCS(p->xc, "width", width, int);
 *   XCS(p->xc, "edge", edge, enum, edge_enum);
 */
#define XCS(xc, name, var, type, extra...)                \
    xconf_set_ ## type(xconf_get(xc, name), var, ## extra)


#endif
