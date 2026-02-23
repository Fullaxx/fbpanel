/*
 * xconf.c -- fbpanel configuration tree: parse, query, update, save.
 *
 * xconf is a simple tree data structure used for fbpanel's config format.
 * Each node has a name, an optional string value, an optional parent, and
 * an ordered list of child nodes ("sons").
 *
 * File format (parsed by read_block / read_line):
 *   - Lines starting with '#' or empty lines are ignored.
 *   - `key = value`       → a leaf node (name="key", value="value").
 *   - `blockname {`       → a block node (name="blockname", value=NULL)
 *                           with sons parsed recursively until `}`.
 *   - `}`                 → ends the current block.
 *
 * Key API:
 *   xconf_new_from_file() — parse a file into an xconf tree.
 *   xconf_find()          — find a named child node (case-insensitive).
 *   xconf_get_str()       — read a string value (non-owning pointer).
 *   xconf_get_strdup()    — read a string value (caller-owned copy).
 *   xconf_get_int()       — read an integer value.
 *   xconf_get_enum()      — read an enum value via name lookup.
 *   xconf_set_value()     — write a string value.
 *   xconf_set_int()       — write an integer value.
 *   xconf_set_enum()      — write an enum value by name.
 *   xconf_del()           — free a node and its sub-tree.
 *   xconf_dup()           — deep-copy a tree.
 *   xconf_cmp()           — compare two trees for differences.
 *   xconf_save_to_file()  — serialise tree back to a file.
 *   xconf_prn()           — debug-print tree to a FILE*.
 *
 * Ownership rules:
 *   - xconf_get_str() sets *val to a RAW POINTER into xconf-owned memory.
 *     Do NOT g_free() this pointer.
 *   - xconf_get_strdup() sets *val to a g_strdup'd copy.  Caller MUST g_free().
 *   - xconf_del() frees the node AND all descendants.  After calling it, any
 *     string pointers obtained via xconf_get_str() are dangling.
 *
 * Known bugs / limitations:
 *   - LINE_LENGTH (256) truncates config values longer than 255 characters.
 *   - read_block() calls printf() + exit(1) on unknown tokens; should log
 *     a warning and continue instead.
 *   - xconf_cmp() returns TRUE when the trees DIFFER (not when they are equal),
 *     which is the opposite of typical compare conventions (TRUE = "different").
 *   - xconf_append() traverses the entire sons list to append (O(n)), but
 *     acceptable for config file sizes.
 */

#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

#include "xconf.h"
#include "panel.h"

//#define DEBUGPRN
#include "dbg.h"


/*
 * Line classification tokens used by read_line().
 *   LINE_NONE        — end of file or blank/comment line (no token produced).
 *   LINE_BLOCK_START — a `name {` line; t[0] = name.
 *   LINE_BLOCK_END   — a lone `}` line.
 *   LINE_VAR         — a `key = value` line; t[0] = key, t[1] = value.
 */
enum { LINE_NONE, LINE_BLOCK_START, LINE_BLOCK_END, LINE_VAR };

/* Maximum characters per config line; values longer than this are silently truncated. */
#define LINE_LENGTH 256

/*
 * line -- internal state for one parsed config line.
 *
 * Fields:
 *   type - one of LINE_NONE / LINE_BLOCK_START / LINE_BLOCK_END / LINE_VAR.
 *   str  - raw line buffer (NUL-terminated; at most LINE_LENGTH-1 chars).
 *   t[0] - pointer into str: the key or block name.
 *   t[1] - pointer into str: the value (LINE_VAR only; undefined otherwise).
 */
typedef struct {
    int type;
    gchar str[LINE_LENGTH];
    gchar *t[2];
} line;


/*
 * xconf_new -- allocate a new xconf node.
 *
 * Parameters:
 *   name  - node name; g_strdup'd.
 *   value - node value (NULL for block nodes); g_strdup'd.
 *
 * Returns: newly allocated xconf node (caller owns it; free with xconf_del).
 */
xconf *xconf_new(gchar *name, gchar *value)
{
    xconf *x;

    x = g_new0(xconf, 1);
    x->name = g_strdup(name);
    x->value = g_strdup(value);   /* g_strdup(NULL) returns NULL */
    return x;
}

/*
 * xconf_append_sons -- move all sons of @src to @dst.
 *
 * Transfers ownership of all son nodes from src to dst.
 * After this call, src->sons == NULL.  src itself is not freed.
 *
 * Parameters:
 *   dst - destination parent node.
 *   src - source node whose sons are moved.
 */
void xconf_append_sons(xconf *dst, xconf *src)
{
    GSList *e;
    xconf *tmp;

    if (!dst || !src)
        return;
    /* update each son's parent pointer to dst */
    for (e = src->sons; e; e = g_slist_next(e))
    {
        tmp = e->data;
        tmp->parent = dst;
    }
    /* concatenate src's sons list onto dst's sons list */
    dst->sons = g_slist_concat(dst->sons, src->sons);
    src->sons = NULL;  /* transfer complete; src has no sons left */
}

/*
 * xconf_append -- add @son as the last child of @parent.
 *
 * Sets son->parent and appends to parent->sons.
 * Note: g_slist_append traverses the entire list — O(n).
 *
 * Parameters:
 *   parent - the node to receive the new child.
 *   son    - the node to add; must not already have a parent.
 */
void xconf_append(xconf *parent, xconf *son)
{
    if (!parent || !son)
        return;
    son->parent = parent;
    /* appending requires traversing all list to the end, which is not
     * efficient, but for v 1.0 it's ok*/
    parent->sons = g_slist_append(parent->sons, son);
}

/*
 * xconf_unlink -- detach @x from its parent's sons list.
 *
 * After this call x->parent == NULL and x is no longer in its parent's
 * sons list.  Does nothing if x has no parent.
 *
 * Parameters:
 *   x - node to detach.
 */
void xconf_unlink(xconf *x)
{
    if (x && x->parent)
    {
        x->parent->sons = g_slist_remove(x->parent->sons, x);
        x->parent = NULL;
    }
}

/*
 * xconf_del -- free @x and its entire sub-tree.
 *
 * Recursively deletes all son nodes, then (if !sons_only) frees x->name,
 * x->value, unlinks x from its parent, and frees x itself.
 *
 * Parameters:
 *   x         - root of sub-tree to delete (may be NULL; no-op in that case).
 *   sons_only - if TRUE, delete all sons but keep x itself alive.
 *               (x->name and x->value are preserved.)
 *
 * WARNING: After this call, any gchar* pointers obtained via
 *   xconf_get_str() on any node in this sub-tree are DANGLING POINTERS.
 */
void xconf_del(xconf *x, gboolean sons_only)
{
    GSList *s;
    xconf *x2;

    if (!x)
        return;
    DBG("%s %s\n", x->name, x->value);
    /* delete all sons recursively */
    for (s = x->sons; s; s = g_slist_delete_link(s, s))
    {
        x2 = s->data;
        x2->parent = NULL;
        xconf_del(x2, FALSE);
    }
    x->sons = NULL;
    if (!sons_only)
    {
        g_free(x->name);
        g_free(x->value);
        xconf_unlink(x);   /* remove from parent's sons list */
        g_free(x);
    }
}

/*
 * xconf_set_value -- set node's value to a g_strdup of @value.
 *
 * Deletes all sons first (since the node becomes a leaf), then replaces value.
 *
 * Parameters:
 *   x     - the node to update.
 *   value - new value string (g_strdup'd; pass NULL to clear).
 */
void xconf_set_value(xconf *x, gchar *value)
{
    xconf_del(x, TRUE);      /* remove any son nodes */
    g_free(x->value);
    x->value = g_strdup(value);
}

/*
 * xconf_set_value_ref -- set node's value to @value directly (no copy).
 *
 * Like xconf_set_value() but takes ownership of @value — the caller must
 * not free it after this call.  The node will g_free() it when replaced.
 *
 * Parameters:
 *   x     - the node to update.
 *   value - new value string; ownership transferred to x.
 */
void xconf_set_value_ref(xconf *x, gchar *value)
{
    xconf_del(x, TRUE);
    g_free(x->value);
    x->value = value;   /* take ownership; do NOT g_strdup */
}

/*
 * xconf_set_int -- set node's value to the decimal string form of @i.
 *
 * Parameters:
 *   x - the node to update.
 *   i - integer value to store.
 */
void xconf_set_int(xconf *x, int i)
{
    xconf_del(x, TRUE);
    g_free(x->value);
    x->value = g_strdup_printf("%d", i);
}

/*
 * xconf_get -- find or create a child node with the given name.
 *
 * If a child named @name already exists in @xc, returns it.
 * Otherwise, creates a new child node (with NULL value), appends it,
 * and returns it.
 *
 * Parameters:
 *   xc   - parent node (may be NULL; returns NULL in that case).
 *   name - name to look for or create.
 *
 * Returns: existing or new child node (never NULL if xc != NULL).
 */
xconf *
xconf_get(xconf *xc, gchar *name)
{
    xconf *ret;

    if (!xc)
        return NULL;
    if ((ret = xconf_find(xc, name, 0)))
        return ret;
    /* not found: create and append */
    ret = xconf_new(name, NULL);
    xconf_append(xc, ret);
    return ret;
}

/*
 * xconf_get_value -- return the node's value string (non-owning).
 *
 * Returns: x->value (may be NULL for block nodes).
 * NOTE: Do NOT g_free() the returned pointer.
 */
gchar *xconf_get_value(xconf *x)
{
    return x->value;
}

/*
 * xconf_prn -- serialise an xconf tree to @fp (for debugging or saving).
 *
 * Recursively writes the tree with n*4-space indent per level.
 * Block nodes are written as `name {\n  sons...\n}`.
 * Leaf nodes are written as `name = value`.
 *
 * Parameters:
 *   fp        - output FILE*.
 *   x         - node to print.
 *   n         - current indent level (call with 0 for root).
 *   sons_only - if TRUE, skip printing x itself and print only its sons
 *               (used by xconf_save_to_file to omit the root wrapper).
 */
void xconf_prn(FILE *fp, xconf *x, int n, gboolean sons_only)
{
    int i;
    GSList *s;
    xconf *x2;

    if (!sons_only)
    {
        for (i = 0; i < n; i++)
            fprintf(fp, "    ");
        fprintf(fp, "%s", x->name);
        if (x->value)
            fprintf(fp, " = %s\n", x->value);   /* leaf: name = value */
        else
            fprintf(fp, " {\n");                 /* block: name { */
        n++;
    }
    /* recurse into sons */
    for (s = x->sons; s; s = g_slist_next(s))
    {
        x2 = s->data;
        xconf_prn(fp, x2, n, FALSE);
    }
    if (!sons_only && !x->value)
    {
        n--;
        for (i = 0; i < n; i++)
            fprintf(fp, "    ");
        fprintf(fp, "}\n");   /* close block */
    }
}

/*
 * xconf_find -- find the @no-th child of @x named @name (case-insensitive).
 *
 * Searches @x's sons list for the @no-th occurrence of a node named @name
 * (0 = first match, 1 = second match, etc.).  Name comparison is
 * case-insensitive (strcasecmp).
 *
 * Parameters:
 *   x    - parent node to search.
 *   name - child name to search for.
 *   no   - 0-based occurrence index.
 *
 * Returns: matching xconf node, or NULL if not found.
 */
xconf *xconf_find(xconf *x, gchar *name, int no)
{
    GSList *s;
    xconf *x2;

    if (!x)
        return NULL;
    for (s = x->sons; s; s = g_slist_next(s))
    {
        x2 = s->data;
        if (!strcasecmp(x2->name, name))   /* case-insensitive match */
        {
            if (!no)
                return x2;   /* found the @no-th match */
            no--;
        }
    }
    return NULL;
}


/*
 * xconf_get_str -- read node value as a non-owning string pointer.
 *
 * Sets *val to point directly into x->value (xconf-owned memory).
 * Does nothing if x is NULL or x->value is NULL.
 *
 * Parameters:
 *   x   - the node to read from.
 *   val - output pointer; set to x->value if it is non-NULL.
 *
 * IMPORTANT: Do NOT g_free(*val) after this call.  Use xconf_get_strdup()
 *            if you need an owned copy.
 */
void xconf_get_str(xconf *x, gchar **val)
{
    if (x && x->value)
        *val = x->value;   /* non-owning pointer; DO NOT g_free */
}


/*
 * xconf_get_strdup -- read node value as a caller-owned g_strdup'd copy.
 *
 * Sets *val to a newly allocated copy of x->value.
 * Does nothing if x is NULL or x->value is NULL.
 *
 * Parameters:
 *   x   - the node to read from.
 *   val - output pointer; set to g_strdup(x->value) if non-NULL.
 *
 * Memory: caller MUST g_free(*val) when done.
 */
void xconf_get_strdup(xconf *x, gchar **val)
{
    if (x && x->value)
        *val = g_strdup(x->value);   /* caller must g_free */
}


/*
 * xconf_get_int -- read node value as an integer (decimal or hex).
 *
 * Parses x->value with strtol(s, NULL, 0) so both decimal ("42") and
 * hex ("0x2a") are accepted.  Does nothing if x is NULL or has no value.
 *
 * Parameters:
 *   x   - the node to read from.
 *   val - output integer; updated only if x has a valid string value.
 */
void xconf_get_int(xconf *x, int *val)
{
    gchar *s;

    if (!x)
        return;
    s = xconf_get_value(x);
    if (!s)
        return;
    *val = strtol(s, NULL, 0);   /* base 0: auto-detect decimal/hex/octal */
}

/*
 * xconf_get_enum -- read node value as an enum integer via a name table.
 *
 * Searches the xconf_enum table @p for a name that matches x->value
 * (case-insensitive).  If found, sets *val to the corresponding numeric code.
 * Does nothing if x is NULL, has no value, or no table entry matches.
 *
 * Parameters:
 *   x   - the node to read from.
 *   val - output integer; updated if a match is found.
 *   p   - NULL-terminated array of xconf_enum entries (str, num pairs).
 */
void xconf_get_enum(xconf *x, int *val, xconf_enum *p)
{
    gchar *s;

    if (!x)
        return;
    s = xconf_get_value(x);
    if (!s)
        return;
    while (p && p->str)
    {
        DBG("cmp %s %s\n", p->str, s);
        if (!strcasecmp(p->str, s))
        {
            *val = p->num;
            return;
        }
        p++;
    }
    /* no match found; *val is unchanged */
}

/*
 * xconf_set_enum -- store an enum value as its string representation.
 *
 * Searches the xconf_enum table @p for the entry with num == @val
 * and calls xconf_set_value(x, str) to store its name.
 * Does nothing if x is NULL or the value is not found in the table.
 *
 * Parameters:
 *   x   - the node to update.
 *   val - numeric enum value to store.
 *   p   - NULL-terminated array of xconf_enum entries.
 */
void
xconf_set_enum(xconf *x, int val, xconf_enum *p)
{
    if (!x)
        return;

    while (p && p->str)
    {
        if (val == p->num)
        {
            xconf_set_value(x, p->str);   /* store the string name */
            return;
        }
        p++;
    }
    /* val not found in table; x unchanged */
}

/*
 * read_line -- read and classify the next non-blank, non-comment line.
 *
 * Reads lines from @fp until a classifiable token is found or EOF.
 * Populates @s->type and @s->t[] based on the line content.
 *
 * Parameters:
 *   fp - open FILE* to read from.
 *   s  - output structure; s->type is set to one of LINE_*.
 *
 * Returns: s->type (LINE_NONE at EOF or for empty/comment-only files).
 *
 * BUG: Unknown tokens (not `}`, not `name = value`, not `name {`) call
 *   ERR() to log the bad character, but then fall through to break — the
 *   s->type is left as LINE_NONE, silently losing the line.  If this is
 *   really a fatal error, it should call exit(); if benign, it should
 *   continue to the next line.
 *
 * Note: LINE_LENGTH is 256; lines longer than 255 chars are silently truncated
 *   by fgets.  Values longer than this cannot be represented.
 */
static int
read_line(FILE *fp, line *s)
{
    gchar *tmp, *tmp2;

    ENTER;
    s->type = LINE_NONE;
    if (!fp)
        RET(s->type);
    while (fgets(s->str, LINE_LENGTH, fp)) {
        g_strstrip(s->str);   /* strip leading/trailing whitespace in-place */

        /* skip blank lines and comment lines */
        if (s->str[0] == '#' || s->str[0] == 0) {
            continue;
        }
        DBG( ">> %s\n", s->str);
        if (!g_ascii_strcasecmp(s->str, "}")) {
            s->type = LINE_BLOCK_END;
            break;
        }

        /* scan past the name (alphanumeric chars) */
        s->t[0] = s->str;
        for (tmp = s->str; isalnum(*tmp); tmp++);
        /* skip whitespace between name and '=' or '{' */
        for (tmp2 = tmp; isspace(*tmp2); tmp2++);
        if (*tmp2 == '=') {
            /* key = value */
            for (++tmp2; isspace(*tmp2); tmp2++);  /* skip space after '=' */
            s->t[1] = tmp2;
            *tmp = 0;     /* NUL-terminate the key in-place */
            s->type = LINE_VAR;
        } else if  (*tmp2 == '{') {
            /* block start */
            *tmp = 0;     /* NUL-terminate the block name in-place */
            s->type = LINE_BLOCK_START;
        } else {
            /* unknown token — log and leave type as LINE_NONE */
            ERR( "parser: unknown token: '%c'\n", *tmp2);
        }
        break;
    }
    RET(s->type);
}


/*
 * read_block -- recursively parse a config block from @fp.
 *
 * Creates a new xconf node named @name and populates it by reading lines
 * until a LINE_BLOCK_END ('}') or EOF.
 *
 * Parameters:
 *   fp   - open FILE* positioned inside a block (after the `{`).
 *   name - name for the new block node.
 *
 * Returns: the newly created xconf node with all parsed children.
 *
 * BUG: calls printf("syntax error\n") + exit(1) for LINE_NONE (which is
 *   also returned at EOF).  This means hitting EOF inside a block causes
 *   an immediate crash rather than a graceful parse error.
 */
static xconf *
read_block(FILE *fp, gchar *name)
{
    line s;
    xconf *x, *xs;

    x = xconf_new(name, NULL);
    while (read_line(fp, &s) != LINE_NONE)
    {
        if (s.type == LINE_BLOCK_START)
        {
            xs = read_block(fp, s.t[0]);   /* recurse into nested block */
            xconf_append(x, xs);
        }
        else if (s.type == LINE_BLOCK_END)
            break;                          /* end of this block */
        else if (s.type == LINE_VAR)
        {
            xs = xconf_new(s.t[0], s.t[1]);
            xconf_append(x, xs);
        }
        else
        {
            /* BUG: exit(1) on LINE_NONE — crashes at unexpected EOF */
            printf("syntax error\n");
            exit(1);
        }
    }
    return x;
}

/*
 * xconf_new_from_file -- parse a config file into an xconf tree.
 *
 * Opens @fname and passes it to read_block() to build a tree rooted
 * at a node named @name.
 *
 * Parameters:
 *   fname - path to the config file.
 *   name  - name for the root node of the resulting tree.
 *
 * Returns: root xconf node (caller owns it; free with xconf_del(ret, FALSE)),
 *          or NULL if the file cannot be opened.
 */
xconf *xconf_new_from_file(gchar *fname, gchar *name)
{
    FILE *fp = fopen(fname, "r");
    xconf *ret = NULL;
    if (fp)
    {
        ret = read_block(fp, name);
        fclose(fp);
    }
    return ret;
}

/*
 * xconf_save_to_file -- serialise xconf tree to a file.
 *
 * Opens @fname for writing (truncates existing content) and writes the
 * tree using xconf_prn() with sons_only=TRUE (omits the root node itself).
 *
 * Parameters:
 *   fname - output file path.
 *   xc    - root xconf node (its sons are written; root itself is omitted).
 */
void xconf_save_to_file(gchar *fname, xconf *xc)
{
    FILE *fp = fopen(fname, "w");

    if (fp)
    {
        xconf_prn(fp, xc, 0, TRUE);   /* sons_only: skip root node header */
        fclose(fp);
    }
}

/*
 * xconf_save_to_profile -- save xconf tree to the active profile file.
 *
 * Delegates to xconf_save_to_file() using panel_get_profile_file().
 *
 * Parameters:
 *   xc - root xconf node to save.
 */
void
xconf_save_to_profile(xconf *xc)
{
    xconf_save_to_file(panel_get_profile_file(), xc);
}

/*
 * xconf_dup -- deep-copy an xconf tree.
 *
 * Recursively duplicates @xc and all its descendants.
 * Each new node has its own g_strdup'd name and value.
 *
 * Parameters:
 *   xc - root of the tree to duplicate (may be NULL; returns NULL).
 *
 * Returns: root of the new tree (caller owns it; free with xconf_del(ret, FALSE)).
 */
xconf *xconf_dup(xconf *xc)
{
    xconf *ret, *son;
    GSList *s;

    if (!xc)
        return NULL;
    ret = xconf_new(xc->name, xc->value);
    for (s = xc->sons; s; s = g_slist_next(s))
    {
        son = s->data;
        xconf_append(ret, xconf_dup(son));   /* recursive deep copy */
    }
    return ret;
}

/*
 * xconf_cmp -- compare two xconf trees for differences.
 *
 * Returns TRUE if the trees DIFFER, FALSE if they are IDENTICAL.
 *
 * NOTE: This is the OPPOSITE of the typical convention (normally 0 = equal).
 *   The return value should be interpreted as "has_differences", not as a
 *   signed comparison result.
 *
 * Comparison checks: name (case-insensitive), value (case-sensitive via
 * g_strcmp0), and all sons recursively in order.  Extra sons in either tree
 * also cause a difference.
 *
 * Parameters:
 *   a - first tree.
 *   b - second tree.
 *
 * Returns: TRUE if trees differ, FALSE if identical.
 *
 * BUG: The first check `if (!(a || b)) return FALSE` is correct (both NULL
 *   = no difference), but the second `if (!(a && b)) return TRUE` handles
 *   the case where exactly one is NULL.  However, this also handles the case
 *   where BOTH are NULL (but that's already caught by the first check).
 */
gboolean
xconf_cmp(xconf *a, xconf *b)
{
    GSList *as, *bs;

    if (!(a || b))     /* both NULL → identical */
        return FALSE;
    if (!(a && b))     /* exactly one NULL → different */
        return TRUE;

    if (g_ascii_strcasecmp(a->name, b->name))   /* names differ */
        return TRUE;

    if (g_strcmp0(a->value, b->value))          /* values differ (case-sensitive) */
        return TRUE;
    /* compare sons pairwise */
    for (as = a->sons, bs = b->sons; as && bs;
         as = g_slist_next(as), bs = g_slist_next(bs))
    {
        if (xconf_cmp(as->data, bs->data))
            return TRUE;
    }
    return (as != bs);   /* one list has extra sons → different */
}
