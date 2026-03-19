# worlds.h

A single-header, pure-C software transactional memory (STM) library.

Ported and extended from the [Common Lisp `worlds` package][lisp-worlds].
The core idea: instead of protecting shared state with locks, you *branch* the
world, make your changes in isolation, then *commit* them back — with automatic
conflict detection.  If two branches diverge on the same data, the second commit
is rejected and can be retried cleanly.

```
root world ──────────────────────────────────────► time
               │           │
           sprout()    sprout()
               │           │
           branch A    branch B   ← both read account.balance = 100
               │           │
           balance=90   balance=80
               │           │
           commit A ✓       │     ← A merges cleanly
                        commit B ✗ ← B read a value A already changed
                        retry B…   ← re-read 90, write 80, commit ✓
```

---

## Features

| | |
|---|---|
| **Branch / commit / retry** | `worlds_sprout`, `worlds_commit`, `worlds_commit_n`, `WORLDS_RETRY` |
| **Conflict detection** | Reads are logged; commit rejects if a concurrent write changed a read value |
| **Force commit** | `worlds_commit_force` — last-writer-wins, no conflict check |
| **Arbitrary forks** | `worlds_fork(parent)` — branch from any world, not just the current one |
| **Snapshots** | `worlds_snapshot` — deep-copy a world's state for checkpointing |
| **Introspection** | `worlds_write_count`, `worlds_read_count`, `worlds_has_write`, `worlds_depth` |
| **RAII cleanup** | `WORLDS_DEFER_FREE` — GCC/Clang `cleanup` attribute |
| **Zero dependencies** | Only C standard library headers |
| **C99 / C11** | C99 minimum; C11 adds thread-safe TLS and `mtx_t` locking |

---

## Getting started

Copy `worlds.h` into your project.  No build step, no separate `.c` file.

```c
#include "worlds.h"
```

Everything is `static inline`.  That is all you need.

---

## Core concepts

### Worlds and the current world

At any point in time each thread has one *active world*.  Reads and writes are
always recorded against the active world.  The **root world** is the global
baseline that every thread starts with.

```
root world  ←  active world at program start (and after worlds_leave)
    └── branch A  ←  active while inside WORLDS_WITH(branch_a)
            └── branch B  ←  active while inside WORLDS_WITH(branch_b)
```

### Slot IDs

Every field you want to track needs a unique `uintptr_t` key called a *slot*.
The `WSLOT` macro derives one from a struct field at compile time:

```c
typedef struct {
    int balance;
    int tx_count;
} Account;

uintptr_t SLOT_BAL = WSLOT(Account, balance);
uintptr_t SLOT_TX  = WSLOT(Account, tx_count);
```

Any unique `uintptr_t` constant works — you are not limited to `offsetof`.

### Values

All values are `uintptr_t` (pointer-width integers).  Store pointers directly,
or any integer that fits.  For larger structs, store a pointer to heap memory
and manage its lifetime outside the library.

`WORLDS_UNBOUND` (`UINTPTR_MAX`) is the sentinel for explicitly unbound slots.

---

## API reference

### World lifecycle

```c
worlds_t *worlds_root(void);
// The global root world. Never free this.

worlds_t *worlds_current(void);
// The calling thread's active world.

worlds_t *worlds_sprout(void);
// Create a child of the current world. Caller must worlds_free() it.

worlds_t *worlds_fork(worlds_t *parent);
// Create a child of any world, not just the current one.

worlds_t *worlds_snapshot(worlds_t *source);
// Deep-copy source's write log into a new standalone world (parent = root).

void worlds_reset(worlds_t *w);
// Clear w's read and write logs; keep its parent. Use for retry loops.

void worlds_discard(worlds_t *w);
// Reset w and set parent = NULL (makes it an orphan).

void worlds_copy_writes_into(worlds_t *dst, worlds_t *src);
// Merge src's write log into dst's. Collisions overwrite.

void worlds_free(worlds_t *w);
// Release all resources. Safe on NULL or the root world (no-op).
```

### Entering and leaving worlds

```c
worlds_t *worlds_enter(worlds_t *w);
// Activate w for this thread. Returns the previous active world.

void worlds_leave(worlds_t *prev);
// Restore prev as the active world.
```

Always pair `worlds_enter` / `worlds_leave`.  For automatic restoration use the
`WORLDS_WITH` macro or `WORLDS_DEFER_FREE`.

### Reading and writing

```c
void  worlds_write(void *object, uintptr_t slot, uintptr_t value);
// Record a write in the current world.

void  worlds_write_in(worlds_t *w, void *object, uintptr_t slot, uintptr_t value);
// Write into a specific world without entering it.

bool  worlds_read(void *object, uintptr_t slot, uintptr_t *out);
// Walk from the current world to root; return true and set *out if found.
// Side-effect: caches ancestor values in the current world's read log.

uintptr_t worlds_read_or(void *object, uintptr_t slot, uintptr_t fallback);
// Read with a fallback; returns fallback if the slot is absent or unbound.

bool  worlds_read_in(worlds_t *w, void *object, uintptr_t slot, uintptr_t *out);
// Full chain-walk read starting from world w (not the current world).

bool  worlds_peek(worlds_t *w, void *object, uintptr_t slot, uintptr_t *out);
// Inspect w's own write log only — no chain walk, no read-log side-effect.

bool  worlds_boundp(void *object, uintptr_t slot);
// true if the slot is bound in the current world or any ancestor.

void  worlds_makunbound(void *object, uintptr_t slot);
// Explicitly unbind a slot in the current world.
```

### Committing

```c
worlds_conflict_t worlds_commit(worlds_t *child);
// Merge child into its parent with conflict detection.
// Returns {.has_conflict = false} on success.
// On conflict, the parent is unchanged and the result describes the clash.

void worlds_commit_force(worlds_t *child);
// Merge unconditionally — child's writes always win.

worlds_conflict_t worlds_commit_n(worlds_t *child,
                                   unsigned max_attempts,
                                   worlds_reset_fn fn_reset,
                                   void *userdata);
// Retry loop: commit, and on conflict call fn_reset then retry.
// max_attempts == 0 means unlimited. fn_reset may be NULL.
```

**Conflict rule**: for every `(object, slot)` that `child` *read*, if the
parent's write log contains a *different* value, the commit is rejected.

### Introspection

```c
size_t worlds_write_count(worlds_t *w);
size_t worlds_read_count(worlds_t *w);
bool   worlds_has_write(worlds_t *w, void *object, uintptr_t slot);
bool   worlds_has_read (worlds_t *w, void *object, uintptr_t slot);
size_t worlds_depth(worlds_t *w);   // 0 for root

void worlds_foreach_write(worlds_t *w, worlds_entry_cb fn, void *userdata);
void worlds_foreach_read (worlds_t *w, worlds_entry_cb fn, void *userdata);
// Callback: void fn(void *object, uintptr_t slot, uintptr_t value, void *userdata)
```

### Macros

```c
WORLDS_WITH(w) { ... }
// Execute the block with w active, then restore the previous world.
// ⚠ Do NOT exit via return/break/goto — use a flag variable instead.

WORLDS_RETRY(branch, max_n, { ... })
// Reset branch, run body inside it, commit; retry up to max_n times on conflict.
// The body is re-executed verbatim on each retry.

worlds_t *w = worlds_sprout();
WORLDS_DEFER_FREE(w) {
    // ... w is freed automatically when this block ends naturally.
    // Portable: no compiler extensions; uses only standard C99 for-loop
    // and __LINE__ token-pasting.
    // ⚠ Same constraint as WORLDS_WITH: free only runs on normal exit,
    //   not via return/break/goto/longjmp.
}
```

---

## Multi-translation-unit programs

By default every `.c` file that includes `worlds.h` gets its own independent
root world (because `worlds_root()` is `static inline` with a local static).
This is fine for single-TU programs.

To share one root world across multiple `.c` files, define `WORLDS_ROOT_IMPL`
in exactly one file before including the header:

```c
/* worlds_root.c — exactly one file */
#define WORLDS_ROOT_IMPL
#include "worlds.h"
```

All other files include normally and will call into that shared definition.

---

## Threading

| Feature | Requirement |
|---|---|
| Thread-local current world | C11 `_Thread_local` (falls back to `__thread`) |
| Mutex on commit | C11 `<threads.h>` `mtx_t` (falls back to no-op) |

`worlds_commit()` holds the parent's mutex for the duration of its conflict
check and write propagation, so concurrent commits from different threads are
safe.  Reads and writes within a single world are not locked — they are designed
to be thread-private.

Define `WORLDS_NO_THREADS` to strip all locking and TLS for single-threaded
builds.

---

## Building the example

```sh
make            # build example (C11, with threads if available)
make run        # build and run
make c99        # build with -std=c99
make clean      # remove build artefacts
make help       # list all targets
```

---

## Limitations

- Values are `uintptr_t`.  Storing values larger than a pointer requires an
  extra heap allocation whose lifetime you manage.
- `worlds_root()` initialisation is not protected by a lock.  Call it once from
  the main thread before spawning workers.
- In C (unlike C++), function-local statics are not shared across translation
  units, which is why `WORLDS_ROOT_IMPL` exists.

---

## Background and bug fixes

This library is a port of the Common Lisp [worlds][lisp-worlds] package by
[Nikodemus Siivola][author].  Two bugs in the original were corrected:

1. **Broken ancestor traversal** — `slot-value-using-class` iterated a loop
   variable `world` but always read from `*current-world*` (unchanged), so only
   the current world was ever checked.  Fixed by reading from the loop variable.

2. **Dead read-propagation guard** — in `commit`, the `puthash2` call for
   propagating reads was placed *outside* the `unless` guard, making the guard
   dead code.  Fixed: reads are propagated only when the parent has not already
   logged that `(object, slot)`.

---

## License

MIT — Copyright (c) 2024.  No warranty; do whatever you like.

[lisp-worlds]: https://github.com/nikodemus/worlds
[author]: https://github.com/nikodemus
