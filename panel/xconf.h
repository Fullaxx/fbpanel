/**
 * @file
 * @brief fbpanel configuration tree API.
 *
 * fbpanel uses a simple text-based configuration format consisting of
 * whitespace-separated key=value pairs nested inside named blocks:
 *
 * @code
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
 * @endcode
 *
 * This module parses that format into a tree of xconf nodes and provides
 * typed accessor macros (::XCG / ::XCS) for reading and writing values.
 *
 * @par Tree structure
 *   Each node has a name, an optional string value, a list of child nodes
 *   (sons), and a back-pointer to its parent. Block nodes (e.g., "Global",
 *   "Plugin") have children but no direct value. Leaf nodes (e.g.,
 *   "edge=bottom") have a value but typically no children.
 *
 * @par Ownership
 *   - xconf nodes are heap-allocated; free with xconf_del().
 *   - name and value strings are owned by the node (g_strdup'd on set;
 *     g_free'd on del).
 *   - Sons are owned by their parent; `xconf_del(parent, FALSE)` recursively
 *     frees all descendants.
 *   - Plugins receive a pointer into the panel's config tree (`p->xc`).
 *     They must NOT free or modify `p->xc` -- it is owned by the panel.
 */
#ifndef _XCONF_H_
#define _XCONF_H_

#include <glib.h>
#include <stdio.h>

/**
 * @brief One node in the configuration tree.
 */
typedef struct _xconf
{
    gchar *name;           /**< Node name (e.g., "edge", "Plugin", "Config"); g_strdup'd; freed by xconf_del(). */
    gchar *value;          /**< Node value string (e.g., "bottom", "dclock"); g_strdup'd. NULL for block nodes that contain only children. */
    GSList *sons;          /**< GSList of child xconf* pointers, in config-file order. Empty list for leaf nodes. */
    struct _xconf *parent; /**< Back-pointer to parent node; NULL for the root node. */
} xconf;

/**
 * @brief String-to-integer mapping for enum-typed config values.
 *
 * Used with xconf_get_enum() / xconf_set_enum() to convert between the
 * string stored in the config file and an integer constant.
 *
 * @note The last entry in an xconf_enum array must have `str == NULL`.
 */
typedef struct {
    gchar *str;   /**< The config-file token (e.g., "top", "bottom", "left", "right"). */
    gchar *desc;  /**< Human-readable description for display in the preferences UI. */
    int num;      /**< The corresponding integer constant (e.g., EDGE_TOP, EDGE_BOTTOM). */
} xconf_enum;

/* --- Tree construction and destruction --- */

/**
 * @brief Allocate a new xconf node.
 * @param name  Node name; g_strdup'd into the node.
 * @param value Node value; g_strdup'd, or NULL for a block node.
 * @return A new xconf* with no parent and an empty sons list. Caller must
 *         free with `xconf_del(node, FALSE)`.
 */
xconf *xconf_new(gchar *name, gchar *value);

/**
 * @brief Add @p son as the last child of @p parent.
 *
 * Sets `son->parent = parent`. Ownership of @p son transfers to @p parent;
 * do not free @p son separately after this call.
 *
 * @param parent The node to append to.
 * @param son    The node to append; ownership transfers to @p parent.
 */
void xconf_append(xconf *parent, xconf *son);

/**
 * @brief Move all children of @p son into @p parent.
 *
 * Used when merging subtrees; after the call, @p son has no children.
 *
 * @param parent Destination node.
 * @param son    Source node; its children are moved out.
 */
void xconf_append_sons(xconf *parent, xconf *son);

/**
 * @brief Detach a node from its parent without freeing it.
 *
 * After this call, `x->parent == NULL` and @p x is no longer in the
 * parent's sons list. Caller now owns @p x.
 *
 * @param x The node to detach.
 */
void xconf_unlink(xconf *x);

/**
 * @brief Free an xconf node and optionally all descendants.
 *
 * @param x         The node to free. Must be unlinked from its parent
 *                  first (or be the root).
 * @param sons_only If TRUE, free only the children (leaving @p x itself
 *                  allocated); if FALSE, free @p x and all descendants
 *                  recursively.
 * @note Frees name, value, and (if @p sons_only is FALSE) the node struct.
 */
void xconf_del(xconf *x, gboolean sons_only);

/* --- Value access --- */

/**
 * @brief Set a node's value string (makes a copy).
 *
 * Frees the old value (if any) and g_strdup()'s the new one. The node
 * takes ownership of the copy; caller keeps its original.
 *
 * @param x     The node to update.
 * @param value New value string (copied).
 */
void xconf_set_value(xconf *x, gchar *value);

/**
 * @brief Set a node's value, taking ownership directly (no copy).
 *
 * Like xconf_set_value() but does NOT copy: `x->value = value`. The
 * string must have been g_malloc'd; the node will g_free() it.
 *
 * @param x     The node to update.
 * @param value New value string; ownership transfers to the node.
 */
void xconf_set_value_ref(xconf *x, gchar *value);

/**
 * @brief Return the node's raw string value.
 * @param x The node to read.
 * @return `x->value` (non-owning); NULL if the node has no value. Do NOT
 *         free the returned pointer.
 */
gchar *xconf_get_value(xconf *x);

/* --- File I/O --- */

/**
 * @brief Serialise a node (and optionally its subtree) to a file.
 * @param fp        Open FILE* to write to.
 * @param x         Root of the subtree to serialise.
 * @param n         Indentation level (0 = top-level, increases with nesting).
 * @param sons_only If TRUE, serialise only children (not @p x's own name/value).
 */
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only);

/**
 * @brief Parse a config file into an xconf tree.
 * @param fname Path to the config file.
 * @param name  Name to assign to the root node of the returned tree.
 * @return The root xconf* of the parsed tree, or NULL on read error.
 *         Caller owns the returned tree; free with `xconf_del(root, FALSE)`.
 */
xconf *xconf_new_from_file(gchar *fname, gchar *name);

/**
 * @brief Serialise a tree to a named file (atomic write).
 * @param fname Destination file path.
 * @param xc    Root of the tree to serialise.
 */
void xconf_save_to_file(gchar *fname, xconf *xc);

/**
 * @brief Save the global panel config to the active profile.
 *
 * Uses `the_panel->profile_name` to determine the file path.
 *
 * @param xc Root of the tree to serialise.
 */
void xconf_save_to_profile(xconf *xc);

/* --- Node lookup --- */

/**
 * @brief Find the Nth child node with a given name.
 * @param x    Parent node to search within (immediate children only).
 * @param name Name to match (case-sensitive).
 * @param no   0-based index among matching nodes (0 = first match).
 * @return The matching xconf*, or NULL if not found. The returned pointer
 *         is non-owning (do not free).
 */
xconf *xconf_find(xconf *x, gchar *name, int no);

/**
 * @brief Deep-copy a subtree.
 * @param xc The subtree to copy.
 * @return A newly allocated xconf* tree that is a complete copy of @p xc.
 *         Caller owns the copy; free with `xconf_del(copy, FALSE)`.
 */
xconf *xconf_dup(xconf *xc);

/**
 * @brief Compare two xconf trees for structural and value equality.
 * @param a First tree.
 * @param b Second tree.
 * @return TRUE if @p a and @p b have equal names, values, and children recursively.
 */
gboolean xconf_cmp(xconf *a, xconf *b);

/* --- Typed accessors (lower-level, prefer XCG/XCS macros) --- */

/**
 * @brief Find or create a named child node.
 *
 * If a child with @p name exists, returns it. If not, creates a new child
 * node with that name and no value. Used by ::XCS to get-or-create the
 * target node before setting a value.
 *
 * @param x    Parent node.
 * @param name Child name to find or create.
 * @return The existing or newly created child node.
 */
xconf *xconf_get(xconf *x, gchar *name);

/**
 * @brief Read the node's value as an integer (atoi).
 * @param x   May be NULL (silently skips if node not found via xconf_find()).
 * @param val Receives the parsed integer. Unchanged if @p x is NULL.
 */
void xconf_get_int(xconf *x, int *val);

/**
 * @brief Read the node's value and map it to an int via a table.
 * @param x   May be NULL.
 * @param val Receives the matched integer from `e[i].num`. Unchanged if no match.
 * @param e   NULL-terminated xconf_enum table.
 */
void xconf_get_enum(xconf *x, int *val, xconf_enum *e);

/**
 * @brief Read the node's value as a non-owning string pointer.
 * @param x   May be NULL.
 * @param val Set to `x->value` (not a copy!). The pointed-to string is
 *            owned by the xconf node. Do NOT g_free(*val).
 */
void xconf_get_str(xconf *x, gchar **val);

/**
 * @brief Read the node's value as a newly allocated copy.
 * @param x   May be NULL.
 * @param val Set to `g_strdup(x->value)`. Caller must g_free(*val).
 */
void xconf_get_strdup(xconf *x, gchar **val);

/** @brief Write an int value as a string. @param x Target node. @param val Value to write. */
void xconf_set_int(xconf *x, int val);
/** @brief Write an enum value as its string token. @param x Target node. @param val Integer value to look up. @param e NULL-terminated xconf_enum table. */
void xconf_set_enum(xconf *x, int val, xconf_enum *e);

/**
 * @def XCG(xc, name, var, type, extra...)
 * @brief Type-safe config READ macro.
 *
 * Expands to `xconf_get_<type>(xconf_find(xc, name, 0), var, ##extra)`.
 *
 * @par Examples
 * @code
 *   int width = 100;
 *   XCG(p->xc, "width", &width, int);
 *
 *   gchar *fmt = NULL;
 *   XCG(p->xc, "ClockFmt", &fmt, str);    // fmt = non-owning ptr into xc
 *
 *   int edge = EDGE_BOTTOM;
 *   XCG(p->xc, "edge", &edge, enum, edge_enum);
 * @endcode
 *
 * @note If the key is not found, xconf_find() returns NULL and
 *       `xconf_get_*` silently leaves `*var` unchanged -- so defaults set
 *       before XCG() are preserved.
 */
#define XCG(xc, name, var, type, extra...)                      \
    xconf_get_ ## type(xconf_find(xc, name, 0), var, ## extra)

/**
 * @def XCS(xc, name, var, type, extra...)
 * @brief Type-safe config WRITE macro.
 *
 * Expands to `xconf_set_<type>(xconf_get(xc, name), var, ##extra)`.
 * xconf_get() creates the node if it doesn't exist.
 *
 * @par Examples
 * @code
 *   XCS(p->xc, "width", width, int);
 *   XCS(p->xc, "edge", edge, enum, edge_enum);
 * @endcode
 */
#define XCS(xc, name, var, type, extra...)                \
    xconf_set_ ## type(xconf_get(xc, name), var, ## extra)


#endif
