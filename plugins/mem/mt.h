
/* mt.h -- X-macro list of /proc/meminfo field names.
 *
 * Usage: define MT_ADD(x) before including this header to expand each
 * entry as desired.  mem.c and mem2.c include this header twice:
 *
 *   First pass — generate the MT_* enum constants:
 *     #undef  MT_ADD
 *     #define MT_ADD(x)  MT_ ## x,
 *     enum { #include "mt.h" MT_NUM };
 *
 *   Second pass — generate the mem_type_t mt[] array:
 *     #undef  MT_ADD
 *     #define MT_ADD(x)  { #x, 0, 0 },
 *     mem_type_t mt[] = { #include "mt.h" };
 *
 * Fields read from /proc/meminfo (values are in kB):
 *   MemTotal  — total usable RAM.
 *   MemFree   — completely idle RAM (not used by anything).
 *   MemShared — RAM shared between processes (informational only).
 *   Slab      — kernel slab allocator cache.
 *   Buffers   — kernel I/O buffer cache.
 *   Cached    — page cache (includes reclaimable memory).
 *   SwapTotal — total swap space.
 *   SwapFree  — unused swap space.
 *
 * "Used" memory is computed as:
 *   MemTotal - (MemFree + Buffers + Cached + Slab)
 */

/* Memory types (MT) to scan in /proc/meminfo */
MT_ADD(MemTotal)
MT_ADD(MemFree)
MT_ADD(MemShared)
MT_ADD(Slab)
MT_ADD(Buffers)
MT_ADD(Cached)

MT_ADD(SwapTotal)
MT_ADD(SwapFree)
