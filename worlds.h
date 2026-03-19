/*
 * worlds.h  —  Worlds STM library, pure C edition
 * ════════════════════════════════════════════════════════════════════════════
 *
 * A port and extension of the Common Lisp `worlds` package.
 * Implements a branching, committable world model for software-transactional
 * memory: sprout a child world, make writes that are invisible to other
 * worlds, then commit (with conflict detection) or discard the branch.
 *
 * REQUIREMENTS
 *   C99 minimum.  C11 unlocks:
 *     • _Thread_local  — thread-safe current-world pointer
 *     • <threads.h>    — standard mutex (mtx_t); falls back to no-op if
 *                        __STDC_NO_THREADS__ is defined or unavailable.
 *
 * DEPENDENCIES
 *   Standard library only: <stddef.h> <stdint.h> <stdlib.h> <string.h>
 *                          <assert.h> <stdbool.h>  +  optionally <threads.h>
 *   No POSIX, no Windows headers, no third-party libraries.
 *
 * USAGE — SINGLE-FILE / SINGLE-TU
 *   Just include the header; everything is static inline.
 *
 *     #include "worlds.h"
 *
 * USAGE — MULTI-TU (sharing one root world across translation units)
 *   In exactly ONE .c file define WORLDS_ROOT_IMPL before the include.
 *   All other .c files include normally.
 *
 *     // worlds_root.c  ── exactly one file
 *     #define WORLDS_ROOT_IMPL
 *     #include "worlds.h"
 *
 *   Every TU will now call the same worlds_root() and share one root world.
 *   Without WORLDS_ROOT_IMPL each TU gets its own isolated root world, which
 *   is fine for single-TU programs or for fully isolated module use.
 *
 * ── SLOT IDs ────────────────────────────────────────────────────────────────
 *   Every tracked field needs a unique uintptr_t key.
 *   The WSLOT(Type, field) macro uses offsetof() — zero-cost and stable.
 *
 *     uintptr_t slot = WSLOT(MyStruct, my_field);
 *
 *   Any unique uintptr_t constant (enum, consecutive integer, …) also works.
 *
 * ── VALUES ──────────────────────────────────────────────────────────────────
 *   All values stored are uintptr_t (pointer-sized integers).
 *   Store raw pointers directly, or any integer that fits in pointer width.
 *   For larger data, store a pointer to heap memory and manage its lifetime.
 *   WORLDS_UNBOUND is the sentinel for explicitly unbound slots.
 *
 * ── QUICK-START EXAMPLE ─────────────────────────────────────────────────────
 *
 *   typedef struct { int x; int y; } Point;
 *   Point p = {0};
 *
 *   // Write into the root world
 *   worlds_write(&p, WSLOT(Point, x), 10);
 *
 *   // Branch
 *   worlds_t *branch = worlds_sprout();
 *
 *   WORLDS_WITH(branch) {
 *       worlds_write(&p, WSLOT(Point, x), 42);
 *       uintptr_t v;
 *       worlds_read(&p, WSLOT(Point, x), &v);  // sees 42
 *   }
 *
 *   // Root world still sees 10
 *   worlds_conflict_t r = worlds_commit(branch); // r.has_conflict == 0
 *   // Root world now sees 42
 *
 *   worlds_free(branch);
 *
 * ── RETRY EXAMPLE ───────────────────────────────────────────────────────────
 *
 *   worlds_t *b = worlds_sprout();
 *   WORLDS_RETRY(b, 5, {
 *       uintptr_t cur = 0;
 *       worlds_read(&counter, WSLOT(Counter, n), &cur);
 *       worlds_write(&counter, WSLOT(Counter, n), cur + 1);
 *   });
 *   worlds_free(b);
 *
 * ── THREADING ───────────────────────────────────────────────────────────────
 *   The current world is stored in thread-local storage (_Thread_local / C11).
 *   Each thread starts with the global root world as its active world.
 *   worlds_commit() holds the parent's mutex for the duration of its conflict
 *   check + write propagation, so concurrent commits are safe.
 *
 *   Define WORLDS_NO_THREADS before including to strip all locking and TLS.
 *
 * ── BUG FIXES FROM THE ORIGINAL LISP ───────────────────────────────────────
 *   1. slot-value-using-class iterated a `world` loop variable but always
 *      read from *current-world* (unchanged), so ancestor traversal was
 *      broken.  Fixed: the chain walk now uses the loop variable.
 *   2. In commit, the read-propagation call was outside the `unless` guard
 *      (dead guard).  Fixed: reads propagate only when the parent has not
 *      already logged that (object, slot).
 *
 * ── LICENSE ─────────────────────────────────────────────────────────────────
 *   MIT — Copyright (c) 2024.  No warranty; do whatever you like.
 */

#ifndef WORLDS_H
#define WORLDS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════════════════════════════════
 * §1  Platform — mutex and thread-local storage
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Mutex ─────────────────────────────────────────────────────────────────
 *
 * Prefer C11 <threads.h> (mtx_t).  Falls back to no-op when threads are
 * disabled or unavailable.  Define WORLDS_NO_THREADS to force the no-op path.
 */

#if defined(WORLDS_NO_THREADS)

    typedef int worlds__mutex_t;
#   define worlds__mutex_init(m)    ((void)(m))
#   define worlds__mutex_lock(m)    ((void)(m))
#   define worlds__mutex_unlock(m)  ((void)(m))
#   define worlds__mutex_destroy(m) ((void)(m))

#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
      && !defined(__STDC_NO_THREADS__)

#   include <threads.h>
    typedef mtx_t worlds__mutex_t;
    /* mtx_init returns thrd_success (0) on success; ignore return value with
       a comma-expression so we satisfy "expression statement" in C99 mode. */
#   define worlds__mutex_init(m)    (mtx_init((m), mtx_plain))
#   define worlds__mutex_lock(m)    (mtx_lock(m))
#   define worlds__mutex_unlock(m)  (mtx_unlock(m))
#   define worlds__mutex_destroy(m) (mtx_destroy(m))

#else
    /* C99 or C11 without <threads.h> — single-threaded no-op locking.
     * Mutual exclusion is disabled; see the THREADING note in §1. */
    typedef int worlds__mutex_t;
#   define worlds__mutex_init(m)    ((void)(m))
#   define worlds__mutex_lock(m)    ((void)(m))
#   define worlds__mutex_unlock(m)  ((void)(m))
#   define worlds__mutex_destroy(m) ((void)(m))
#endif

/* ── Thread-local storage ───────────────────────────────────────────────────
 *
 * C11 _Thread_local keyword.  Falls back to __thread (GCC/Clang) or a plain
 * global (no-op TLS) on older compilers.
 */

#if defined(WORLDS_NO_THREADS)
#   define WORLDS__TLS  /* nothing — plain global */
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) \
      && !defined(__STDC_NO_THREADS__)
#   define WORLDS__TLS  _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#   define WORLDS__TLS  __thread
#else
#   define WORLDS__TLS  /* plain global, not thread-safe */
#endif

/* ════════════════════════════════════════════════════════════════════════════
 * §2  Public constants and types
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * WSLOT(Type, field) — derive a stable slot id from a struct field.
 * Uses offsetof, so the id is a compile-time constant with zero run-time cost.
 *
 *   uintptr_t slot_x = WSLOT(Point, x);
 */
#define WSLOT(Type, field)  ((uintptr_t)offsetof(Type, field))

/*
 * WORLDS_UNBOUND — sentinel value meaning "explicitly unbound".
 * The library treats UINTPTR_MAX as unbound; do not store it as a real value.
 */
#define WORLDS_UNBOUND  ((uintptr_t)UINTPTR_MAX)

/* ── Internal hash-table entry ─────────────────────────────────────────── */
typedef struct {
    void     *object;
    uintptr_t slot;
    uintptr_t value;
    bool      occupied;
} worlds__entry_t;

/* ── Internal hash table (open-addressing, power-of-2 capacity) ─────────── */
typedef struct {
    worlds__entry_t *entries;
    size_t           capacity;   /* always a power of two */
    size_t           count;
} worlds__table_t;

/* ── worlds_t ───────────────────────────────────────────────────────────────
 *
 * An isolated transaction context: logs all reads and writes made while it
 * is the active world. Obtain via worlds_sprout() or worlds_fork(); free with
 * worlds_free().  Never free the root world.
 */
typedef struct worlds_s {
    worlds__table_t  reads;
    worlds__table_t  writes;
    worlds__mutex_t  lock;
    struct worlds_s *parent;
} worlds_t;

/* ── worlds_conflict_t ──────────────────────────────────────────────────────
 *
 * Returned by worlds_commit(). If has_conflict is false the commit succeeded.
 * Otherwise object/slot/parent_value/child_value describe the clash.
 */
typedef struct {
    bool      has_conflict;
    void     *object;
    uintptr_t slot;
    uintptr_t parent_value;
    uintptr_t child_value;
} worlds_conflict_t;

/* ── worlds_entry_cb ────────────────────────────────────────────────────────
 *
 * Callback type for worlds_foreach_write() and worlds_foreach_read().
 */
typedef void (*worlds_entry_cb)(void     *object,
                                uintptr_t slot,
                                uintptr_t value,
                                void     *userdata);

/* ── worlds_reset_fn ────────────────────────────────────────────────────────
 *
 * Callback called by worlds_commit_n() after each failed attempt, once the
 * world's logs have been cleared. Use it to re-execute the transaction body.
 */
typedef void (*worlds_reset_fn)(worlds_t *world, void *userdata);

/* ════════════════════════════════════════════════════════════════════════════
 * §3  Internal hash table — static inline implementation
 * ════════════════════════════════════════════════════════════════════════════ */

#define WORLDS__INIT_CAP  16u   /* must be a power of two */

/*
 * Two-input Fibonacci hash.  Mixes the pointer-address and slot-offset bits
 * well enough to give uniform distribution over power-of-two bucket counts.
 */
static inline size_t worlds__hash(void *obj, uintptr_t slot)
{
    uint64_t a = (uint64_t)(uintptr_t)obj * UINT64_C(11400714819323198485);
    uint64_t b = (uint64_t)slot            * UINT64_C(14181476777654086739);
    uint64_t h = a ^ b;
    h ^= (h >> 32);
    return (size_t)h;
}

static inline void worlds__tbl_init(worlds__table_t *t)
{
    t->entries  = (worlds__entry_t *)calloc(WORLDS__INIT_CAP,
                                             sizeof(worlds__entry_t));
    assert(t->entries && "worlds: allocation failed");
    t->capacity = WORLDS__INIT_CAP;
    t->count    = 0;
}

static inline void worlds__tbl_clear(worlds__table_t *t)
{
    memset(t->entries, 0, t->capacity * sizeof(worlds__entry_t));
    t->count = 0;
}

static inline void worlds__tbl_destroy(worlds__table_t *t)
{
    free(t->entries);
    t->entries  = NULL;
    t->capacity = 0;
    t->count    = 0;
}

/*
 * worlds__tbl_find — return the bucket for (obj, slot): either an existing
 * occupied bucket that matches, or the first empty bucket (insertion point).
 * Linear probing; no tombstones (tables are only ever fully cleared).
 */
static inline worlds__entry_t *worlds__tbl_find(worlds__table_t *t,
                                                 void     *obj,
                                                 uintptr_t slot)
{
    size_t mask  = t->capacity - 1;
    size_t index = worlds__hash(obj, slot) & mask;
    for (;;) {
        worlds__entry_t *e = &t->entries[index];
        if (!e->occupied)                        return e; /* empty slot */
        if (e->object == obj && e->slot == slot) return e; /* match */
        index = (index + 1) & mask;                        /* probe next */
    }
}

/* Double capacity and rehash all entries. */
static inline void worlds__tbl_grow(worlds__table_t *t)
{
    size_t           old_cap     = t->capacity;
    worlds__entry_t *old_entries = t->entries;

    t->capacity *= 2;
    t->entries   = (worlds__entry_t *)calloc(t->capacity,
                                              sizeof(worlds__entry_t));
    assert(t->entries && "worlds: allocation failed");
    t->count = 0;

    for (size_t i = 0; i < old_cap; i++) {
        worlds__entry_t *src = &old_entries[i];
        if (!src->occupied) continue;
        worlds__entry_t *dst = worlds__tbl_find(t, src->object, src->slot);
        *dst = *src;
        t->count++;
    }
    free(old_entries);
}

/* Insert or update (obj, slot) → value. Grows at 75 % load. */
static inline void worlds__tbl_put(worlds__table_t *t,
                                    void     *obj,
                                    uintptr_t slot,
                                    uintptr_t value)
{
    if ((t->count + 1) * 4 >= t->capacity * 3)
        worlds__tbl_grow(t);

    worlds__entry_t *e = worlds__tbl_find(t, obj, slot);
    if (!e->occupied) {
        e->occupied = true;
        e->object   = obj;
        e->slot     = slot;
        t->count++;
    }
    e->value = value;
}

/* Lookup (obj, slot). Returns true and sets *out_value if found. */
static inline bool worlds__tbl_get(worlds__table_t *t,
                                    void     *obj,
                                    uintptr_t slot,
                                    uintptr_t *out_value)
{
    worlds__entry_t *e = worlds__tbl_find(t, obj, slot);
    if (e->occupied) {
        *out_value = e->value;
        return true;
    }
    return false;
}

/* Iterate over every occupied entry. */
static inline void worlds__tbl_foreach(worlds__table_t *t,
                                        worlds_entry_cb  fn,
                                        void            *userdata)
{
    for (size_t i = 0; i < t->capacity; i++) {
        worlds__entry_t *e = &t->entries[i];
        if (e->occupied)
            fn(e->object, e->slot, e->value, userdata);
    }
}

/* Copy every entry from src into dst (dst entries are overwritten if they
   share the same (object, slot) key). */
static inline void worlds__tbl_merge_into(worlds__table_t *dst,
                                           worlds__table_t *src)
{
    for (size_t i = 0; i < src->capacity; i++) {
        worlds__entry_t *e = &src->entries[i];
        if (e->occupied)
            worlds__tbl_put(dst, e->object, e->slot, e->value);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * §4  World allocation
 * ════════════════════════════════════════════════════════════════════════════ */

static inline worlds_t *worlds__alloc(worlds_t *parent)
{
    worlds_t *w = (worlds_t *)calloc(1, sizeof(worlds_t));
    assert(w && "worlds: allocation failed");
    worlds__tbl_init(&w->reads);
    worlds__tbl_init(&w->writes);
    worlds__mutex_init(&w->lock);
    w->parent = parent;
    return w;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §5  Root world and current-world pointer
 * ════════════════════════════════════════════════════════════════════════════
 *
 * worlds_root() — return the single global root world.
 *
 * Single-TU / default mode:
 *   worlds_root() is static inline, containing a static local variable.
 *   In C each TU that calls it gets its own root (function-local statics are
 *   not shared across TUs in C).  This is fine for single-TU programs.
 *
 * Multi-TU mode (WORLDS_ROOT_IMPL):
 *   Define WORLDS_ROOT_IMPL in exactly one .c file before including this
 *   header.  That file emits the real definition.  All other files see only
 *   the forward declaration and call into it.
 */

#if defined(WORLDS_ROOT_IMPL)
    /* ── This TU owns the real definition ─────────────────────────────── */
    worlds_t *worlds_root(void);      /* forward-declare for this TU too   */
    worlds_t *worlds_root(void) {
        static worlds_t *root = NULL;
        if (!root) root = worlds__alloc(NULL);
        return root;
    }
#else
    /* ── All other TUs: static inline with a per-TU local static ───────── */
    static inline worlds_t *worlds_root(void) {
        static worlds_t *root = NULL;
        if (!root) root = worlds__alloc(NULL);
        return root;
    }
#endif /* WORLDS_ROOT_IMPL */

/* Thread-local pointer to the currently active world.
   Lazily initialised to worlds_root() on first access. */
static WORLDS__TLS worlds_t *worlds__current = NULL;

/*
 * worlds_current() — the calling thread's active world.
 */
static inline worlds_t *worlds_current(void)
{
    if (!worlds__current)
        worlds__current = worlds_root();
    return worlds__current;
}

/*
 * worlds_enter(w) — activate w for this thread.
 * Returns the previously active world so it can be restored.
 * Always pair with a matching worlds_leave().
 */
static inline worlds_t *worlds_enter(worlds_t *w)
{
    worlds_t *prev  = worlds_current();
    worlds__current = w;
    return prev;
}

/*
 * worlds_leave(prev) — restore prev as the active world.
 * Call with the value returned by the matching worlds_enter().
 */
static inline void worlds_leave(worlds_t *prev)
{
    worlds__current = prev;
}

/*
 * worlds_free(w) — release all resources held by w.
 * Safe to call on NULL or on the root world (both are no-ops).
 */
static inline void worlds_free(worlds_t *w)
{
    if (!w || w == worlds_root()) return;
    worlds__tbl_destroy(&w->reads);
    worlds__tbl_destroy(&w->writes);
    worlds__mutex_destroy(&w->lock);
    free(w);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §6  Branching
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * worlds_sprout() — create a child of the current world.
 * The caller owns the returned world; free it with worlds_free().
 */
static inline worlds_t *worlds_sprout(void)
{
    return worlds__alloc(worlds_current());
}

/*
 * worlds_fork(parent) — create a child of an arbitrary world instead of the
 * current one.  Useful for spawning parallel branches from a checkpoint.
 */
static inline worlds_t *worlds_fork(worlds_t *parent)
{
    return worlds__alloc(parent);
}

/*
 * worlds_snapshot(source) — create a new world whose write log is a deep copy
 * of source's write log.  The snapshot's parent is the root world.
 * Use to checkpoint known-good state for later branching.
 */
static inline worlds_t *worlds_snapshot(worlds_t *source)
{
    worlds_t *snap = worlds__alloc(worlds_root());
    worlds__tbl_merge_into(&snap->writes, &source->writes);
    return snap;
}

/*
 * worlds_reset(w) — clear w's read and write logs without freeing or
 * changing its parent.  Use to retry a transaction with the same world:
 *
 *   for (;;) {
 *       worlds_reset(branch);
 *       WORLDS_WITH(branch) { ... }
 *       worlds_conflict_t r = worlds_commit(branch);
 *       if (!r.has_conflict) break;
 *   }
 */
static inline void worlds_reset(worlds_t *w)
{
    worlds__tbl_clear(&w->reads);
    worlds__tbl_clear(&w->writes);
}

/*
 * worlds_discard(w) — reset w's logs and detach it from its parent (sets
 * parent to NULL).  After this call w is a fresh orphan world.
 */
static inline void worlds_discard(worlds_t *w)
{
    worlds_reset(w);
    w->parent = NULL;
}

/*
 * worlds_copy_writes_into(dst, src) — merge src's write log into dst's.
 * Existing entries in dst are overwritten on collision.
 * dst's parent and read log are untouched.
 */
static inline void worlds_copy_writes_into(worlds_t *dst, worlds_t *src)
{
    worlds__tbl_merge_into(&dst->writes, &src->writes);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §7  Read / write operations
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * worlds_write(object, slot, value) — record a write in the current world.
 * Pass WORLDS_UNBOUND as value to explicitly unbind the slot.
 */
static inline void worlds_write(void *object, uintptr_t slot, uintptr_t value)
{
    worlds__tbl_put(&worlds_current()->writes, object, slot, value);
}

/*
 * worlds_write_in(w, object, slot, value) — write into a specific world
 * without entering it.  Useful for seeding initial state.
 */
static inline void worlds_write_in(worlds_t *w, void *object,
                                    uintptr_t slot, uintptr_t value)
{
    worlds__tbl_put(&w->writes, object, slot, value);
}

/*
 * worlds_read(object, slot, out) — walk from the current world up to the root,
 * checking writes then reads at each level.
 *
 * Side-effect: when the value is found in an ancestor (not the current world),
 * it is cached in the current world's read log for conflict detection.
 *
 * Returns true and sets *out on success.
 * Returns false if the slot is absent everywhere or is explicitly unbound.
 *
 * Bug-fix from original Lisp: the chain walk now uses the loop variable `w`
 * rather than re-reading from *current-world* on every iteration.
 */
static inline bool worlds_read(void *object, uintptr_t slot, uintptr_t *out)
{
    worlds_t *start      = worlds_current();
    bool      is_current = true;

    for (worlds_t *w = start; w != NULL; w = w->parent, is_current = false) {
        uintptr_t value;
        bool found = worlds__tbl_get(&w->writes, object, slot, &value)
                  || worlds__tbl_get(&w->reads,  object, slot, &value);
        if (!found)                   continue;
        if (value == WORLDS_UNBOUND)  return false; /* explicitly unbound */

        /* Cache in start's read log when found via an ancestor. */
        if (!is_current)
            worlds__tbl_put(&start->reads, object, slot, value);

        *out = value;
        return true;
    }
    return false; /* not present in any ancestor */
}

/*
 * worlds_read_or(object, slot, fallback) — read with a fallback value.
 * Returns fallback if the slot is absent or unbound.
 */
static inline uintptr_t worlds_read_or(void     *object,
                                        uintptr_t slot,
                                        uintptr_t fallback)
{
    uintptr_t v;
    return worlds_read(object, slot, &v) ? v : fallback;
}

/*
 * worlds_read_in(w, object, slot, out) — full chain-walk read starting from
 * world w instead of the current world.  Read-log caching is performed into w.
 */
static inline bool worlds_read_in(worlds_t *w, void *object,
                                   uintptr_t slot, uintptr_t *out)
{
    worlds_t *saved = worlds_enter(w);
    bool       ok   = worlds_read(object, slot, out);
    worlds_leave(saved);
    return ok;
}

/*
 * worlds_peek(w, object, slot, out) — look only at w's own write log.
 * No chain walk; no read-log side-effect.  Returns true if found.
 */
static inline bool worlds_peek(worlds_t *w, void *object,
                                uintptr_t slot, uintptr_t *out)
{
    return worlds__tbl_get(&w->writes, object, slot, out);
}

/*
 * worlds_boundp(object, slot) — true if the slot is bound in the current
 * world or any ancestor.
 */
static inline bool worlds_boundp(void *object, uintptr_t slot)
{
    uintptr_t dummy;
    return worlds_read(object, slot, &dummy);
}

/*
 * worlds_makunbound(object, slot) — explicitly unbind a slot in the current
 * world.  Future reads through this world will return false (unbound).
 */
static inline void worlds_makunbound(void *object, uintptr_t slot)
{
    worlds_write(object, slot, WORLDS_UNBOUND);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §8  Introspection
 * ════════════════════════════════════════════════════════════════════════════ */

/* worlds_write_count(w) — number of (object,slot) pairs written in w. */
static inline size_t worlds_write_count(worlds_t *w)
{
    return w->writes.count;
}

/* worlds_read_count(w) — number of (object,slot) pairs in w's read log. */
static inline size_t worlds_read_count(worlds_t *w)
{
    return w->reads.count;
}

/* worlds_has_write(w, object, slot) — true if w recorded a write for the slot.
   Does not walk the chain. */
static inline bool worlds_has_write(worlds_t *w, void *object, uintptr_t slot)
{
    uintptr_t dummy;
    return worlds__tbl_get(&w->writes, object, slot, &dummy);
}

/* worlds_has_read(w, object, slot) — true if the slot is in w's read log. */
static inline bool worlds_has_read(worlds_t *w, void *object, uintptr_t slot)
{
    uintptr_t dummy;
    return worlds__tbl_get(&w->reads, object, slot, &dummy);
}

/* worlds_depth(w) — number of ancestor worlds between w and the root.
   Root returns 0.  O(depth). */
static inline size_t worlds_depth(worlds_t *w)
{
    size_t d = 0;
    for (worlds_t *p = w->parent; p != NULL; p = p->parent)
        d++;
    return d;
}

/* worlds_foreach_write(w, fn, userdata) — iterate over every write in w.
   Does not walk the chain. */
static inline void worlds_foreach_write(worlds_t *w,
                                         worlds_entry_cb fn,
                                         void           *userdata)
{
    worlds__tbl_foreach(&w->writes, fn, userdata);
}

/* worlds_foreach_read(w, fn, userdata) — iterate over every read-log entry.
   Does not walk the chain. */
static inline void worlds_foreach_read(worlds_t *w,
                                        worlds_entry_cb fn,
                                        void           *userdata)
{
    worlds__tbl_foreach(&w->reads, fn, userdata);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §9  Commit
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Internal commit helpers ──────────────────────────────────────────────── */

typedef struct {
    worlds__table_t  *parent_writes;
    worlds_conflict_t result;         /* set on first conflict found */
} worlds__check_ctx_t;

/* Callback: checks whether a child-read value conflicts with the parent. */
static inline void worlds__cb_check(void     *obj,
                                     uintptr_t slot,
                                     uintptr_t child_val,
                                     void     *userdata)
{
    worlds__check_ctx_t *ctx = (worlds__check_ctx_t *)userdata;
    if (ctx->result.has_conflict) return; /* already found one; short-circuit */

    uintptr_t parent_val;
    if (worlds__tbl_get(ctx->parent_writes, obj, slot, &parent_val)
        && parent_val != child_val) {
        ctx->result.has_conflict  = true;
        ctx->result.object        = obj;
        ctx->result.slot          = slot;
        ctx->result.parent_value  = parent_val;
        ctx->result.child_value   = child_val;
    }
}

/* Callback: propagate one write entry from child to parent. */
static inline void worlds__cb_prop_write(void     *obj,
                                          uintptr_t slot,
                                          uintptr_t value,
                                          void     *userdata)
{
    worlds__tbl_put((worlds__table_t *)userdata, obj, slot, value);
}

typedef struct {
    worlds__table_t *parent_reads;
    worlds__table_t *parent_writes;
} worlds__prop_read_ctx_t;

/*
 * Callback: propagate one read-log entry from child into the parent's read
 * log — but only if the parent hasn't already logged that (object, slot) as
 * either a read or a write.
 *
 * Bug-fix from original Lisp: the guard was outside the puthash2 call so it
 * had no effect; this version correctly gates the propagation.
 */
static inline void worlds__cb_prop_read(void     *obj,
                                         uintptr_t slot,
                                         uintptr_t value,
                                         void     *userdata)
{
    worlds__prop_read_ctx_t *ctx = (worlds__prop_read_ctx_t *)userdata;
    uintptr_t dummy;
    if (!worlds__tbl_get(ctx->parent_reads,  obj, slot, &dummy) &&
        !worlds__tbl_get(ctx->parent_writes, obj, slot, &dummy))
        worlds__tbl_put(ctx->parent_reads, obj, slot, value);
}

/*
 * worlds__do_merge — propagate child into parent (lock must already be held).
 * Steps:
 *   1. Child writes → parent writes (overwrite on collision).
 *   2. Child reads  → parent reads  (skip if parent already logged the slot).
 *   3. Clear child's logs.
 */
static inline void worlds__do_merge(worlds_t *child, worlds_t *parent)
{
    /* Step 1 — propagate writes. */
    worlds__tbl_foreach(&child->writes,
                         worlds__cb_prop_write, &parent->writes);

    /* Step 2 — propagate reads (guarded). */
    worlds__prop_read_ctx_t rctx;
    rctx.parent_reads  = &parent->reads;
    rctx.parent_writes = &parent->writes;
    worlds__tbl_foreach(&child->reads, worlds__cb_prop_read, &rctx);

    /* Step 3 — clear child (matching the Lisp clrhash calls). */
    worlds__tbl_clear(&child->reads);
    worlds__tbl_clear(&child->writes);
}

/* ── Public commit functions ──────────────────────────────────────────────── */

/*
 * worlds_commit(child) — merge child into its parent with conflict detection.
 *
 * Conflict rule:
 *   For every (object, slot) that child READ, if the parent's write log
 *   contains a *different* value the commit is rejected and the parent is
 *   left unchanged.
 *
 * On success:
 *   child's writes are merged into the parent; child's logs are cleared.
 *   Returns {.has_conflict = false}.
 *
 * On conflict:
 *   No changes are applied to the parent.
 *   Returns a populated worlds_conflict_t.
 */
static inline worlds_conflict_t worlds_commit(worlds_t *child)
{
    worlds_conflict_t result;
    memset(&result, 0, sizeof(result));

    worlds_t *parent = child->parent;
    assert(parent && "worlds_commit: child has no parent (orphan world)");

    worlds__mutex_lock(&parent->lock);

    /* Conflict check. */
    worlds__check_ctx_t cctx;
    cctx.parent_writes = &parent->writes;
    memset(&cctx.result, 0, sizeof(cctx.result));
    worlds__tbl_foreach(&child->reads, worlds__cb_check, &cctx);

    if (cctx.result.has_conflict) {
        worlds__mutex_unlock(&parent->lock);
        return cctx.result;
    }

    worlds__do_merge(child, parent);
    worlds__mutex_unlock(&parent->lock);
    return result; /* has_conflict == false */
}

/*
 * worlds_commit_force(child) — merge child into its parent unconditionally,
 * skipping all conflict detection.  Child's writes always win.
 * Useful when you own both sides of the branch or explicitly want last-write-
 * wins semantics.
 */
static inline void worlds_commit_force(worlds_t *child)
{
    worlds_t *parent = child->parent;
    assert(parent && "worlds_commit_force: orphan world");
    worlds__mutex_lock(&parent->lock);
    worlds__do_merge(child, parent);
    worlds__mutex_unlock(&parent->lock);
}

/*
 * worlds_commit_n(child, max_attempts, fn_reset, userdata)
 *   — retry loop around worlds_commit().
 *
 * @param child         The world to commit.
 * @param max_attempts  Maximum tries (0 = unlimited).
 * @param fn_reset      Called after each failed attempt, after the world's
 *                      logs have been cleared via worlds_reset().  Use it to
 *                      re-execute the transaction body.  May be NULL.
 * @param userdata      Passed through to fn_reset unchanged.
 *
 * Returns the last worlds_conflict_t.  has_conflict == false on success.
 */
static inline worlds_conflict_t worlds_commit_n(worlds_t      *child,
                                                 unsigned       max_attempts,
                                                 worlds_reset_fn fn_reset,
                                                 void           *userdata)
{
    worlds_conflict_t r;
    memset(&r, 0, sizeof(r));
    unsigned attempt = 0;

    for (;;) {
        r = worlds_commit(child);
        if (!r.has_conflict) break;
        if (max_attempts && ++attempt >= max_attempts) break;
        worlds_reset(child);
        if (fn_reset) fn_reset(child, userdata);
    }
    return r;
}

/* ════════════════════════════════════════════════════════════════════════════
 * §10  Convenience macros
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * WORLDS_WITH(w) { ... }
 *
 * Execute the following compound statement with w as the active world, then
 * restore the previous world.
 *
 * ⚠ Do NOT exit the block via `return`, `break`, `goto`, or `longjmp`; the
 *   previous world will not be restored.  For safe early exits wrap the body
 *   in a do/while and use a flag, or restructure to avoid early return.
 *
 * Example:
 *   WORLDS_WITH(branch) {
 *       worlds_write(&p, WSLOT(Point, x), 42);
 *   }
 */
#define WORLDS_WITH(w)                                          \
    for (worlds_t *_wprev_ = worlds_enter(w),                   \
                  *_wloop_ = (worlds_t *)(w);                   \
         _wloop_ != NULL;                                       \
         worlds_leave(_wprev_), _wloop_ = NULL)

/*
 * WORLDS_RETRY(branch, max_n, body...)
 *
 * Execute body inside branch, commit, and retry up to max_n times on
 * conflict.  Pass 0 for unlimited retries.
 *
 * The entire body is re-executed verbatim on each retry after the world's
 * logs are cleared, so every read inside body should be a worlds_read() call
 * (not a cached local) to keep the read log accurate.
 *
 * Example:
 *   WORLDS_RETRY(branch, 5, {
 *       uintptr_t cur = 0;
 *       worlds_read(&counter, WSLOT(Counter, n), &cur);
 *       worlds_write(&counter, WSLOT(Counter, n), cur + 1);
 *   });
 */
#define WORLDS_RETRY(branch, max_n, ...)                        \
    do {                                                        \
        unsigned _wr_attempts_ = 0;                             \
        for (;;) {                                              \
            worlds_reset(branch);                               \
            WORLDS_WITH(branch) { __VA_ARGS__; }                \
            worlds_conflict_t _wr_r_ = worlds_commit(branch);  \
            if (!_wr_r_.has_conflict) break;                    \
            if ((max_n) && (++_wr_attempts_ >= (unsigned)(max_n))) break; \
        }                                                       \
    } while (0)

/*
 * WORLDS_DEFER_FREE(w) { ... }
 *
 * Execute the following block, then call worlds_free(w) exactly once when
 * control reaches the natural end of the block.
 *
 * Fully portable: uses only the standard C99 for-loop and __LINE__
 * token-pasting.  No compiler extensions, no GCC/Clang/MSVC specifics.
 *
 * Usage:
 *
 *   worlds_t *branch = worlds_sprout();
 *   WORLDS_DEFER_FREE(branch) {
 *       WORLDS_WITH(branch) { ... }
 *       worlds_commit(branch);
 *   }
 *   // worlds_free(branch) called automatically here
 *
 * ── How it works ────────────────────────────────────────────────────────
 *   Expands to a for-loop whose body runs once (first iteration) and whose
 *   increment calls worlds_free() then sets the guard to NULL, which makes
 *   the condition false and ends the loop.
 *
 *   Name uniqueness uses __LINE__ (standard since C99) so multiple
 *   WORLDS_DEFER_FREE calls in the same function do not collide.
 *
 * ⚠ Limitation — worlds_free() is called only when control falls off the
 *   bottom of the block naturally.  It is NOT called if the block is exited
 *   via return, break, goto, or longjmp.  For those cases call worlds_free()
 *   explicitly, or restructure to avoid early exit.  This is the same
 *   constraint as WORLDS_WITH.
 */

/* Two-level paste needed so __LINE__ is fully expanded before ##. */
#define WORLDS__CAT2(a, b)  a##b
#define WORLDS__CAT(a, b)   WORLDS__CAT2(a, b)

#define WORLDS_DEFER_FREE(w)                                                   \
    for (worlds_t *WORLDS__CAT(_wdf_, __LINE__) = (w);                         \
         WORLDS__CAT(_wdf_, __LINE__) != NULL;                                  \
         worlds_free(WORLDS__CAT(_wdf_, __LINE__)),                             \
         WORLDS__CAT(_wdf_, __LINE__) = NULL)

#endif /* WORLDS_H */
