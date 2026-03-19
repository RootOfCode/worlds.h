/*
 * example.c — worlds.h demonstration
 * ════════════════════════════════════════════════════════════════════════════
 *
 * Scenario: a small bank ledger with three accounts.
 *
 * The program walks through the core ideas one at a time:
 *
 *   §1  Basic write / read                  — the simplest possible usage
 *   §2  Branching and isolation             — writes in a branch are invisible
 *                                             to the root until committed
 *   §3  Conflict detection                  — two branches diverge; the second
 *                                             commit is correctly rejected
 *   §4  Retry with WORLDS_RETRY             — automatic retry resolves conflict
 *   §5  Force commit                        — last-writer-wins, no check
 *   §6  Snapshot / audit                    — checkpoint state for later reads
 *   §7  worlds_commit_n with a callback     — manual retry with a reset hook
 *   §8  Introspection helpers               — depth, counts, foreach
 *   §9  WORLDS_DEFER_FREE                   — automatic cleanup (GCC/Clang)
 *
 * Build:
 *   make          (C11)
 *   make c99      (C99)
 *   make run
 */

#include "worlds.h"
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════════
 * Data model
 * ════════════════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *name;
    int         balance;   /* tracked */
    int         tx_count;  /* tracked */
} Account;

/*
 * Helper: read an integer field from the current world.
 * worlds_read() stores the raw uintptr_t; we copy the bytes back to int.
 */
static int account_get(Account *acc, uintptr_t slot)
{
    uintptr_t raw;
    if (!worlds_read(acc, slot, &raw)) {
        fprintf(stderr, "account_get: slot unbound for %s\n", acc->name);
        return -1;
    }
    int v;
    memcpy(&v, &raw, sizeof(v));
    return v;
}

/* Write an integer field into the current world. */
static void account_set(Account *acc, uintptr_t slot, int value)
{
    uintptr_t raw = 0;
    memcpy(&raw, &value, sizeof(value));
    worlds_write(acc, slot, raw);
}

/* Convenience: read balance / tx_count in the current world. */
static int  balance(Account *acc) { return account_get(acc, WSLOT(Account, balance));  }
static int  txcount(Account *acc) { return account_get(acc, WSLOT(Account, tx_count)); }

/* Initialise an account in the current (root) world. */
static void account_init(Account *acc, const char *name, int bal)
{
    acc->name = name;
    account_set(acc, WSLOT(Account, balance),  bal);
    account_set(acc, WSLOT(Account, tx_count), 0);
}

/* Print one account's state as seen by the current world. */
static void account_print(Account *acc)
{
    printf("  %-10s  balance=%4d  tx=%d\n",
           acc->name, balance(acc), txcount(acc));
}

/* Transfer `amount` from `from` to `to` in the current world. */
static void transfer(Account *from, Account *to, int amount)
{
    account_set(from, WSLOT(Account, balance), balance(from) - amount);
    account_set(to,   WSLOT(Account, balance), balance(to)   + amount);
    account_set(from, WSLOT(Account, tx_count), txcount(from) + 1);
    account_set(to,   WSLOT(Account, tx_count), txcount(to)   + 1);
}

/* Print a labelled separator. */
static void section(const char *title)
{
    printf("\n── %s ", title);
    for (int i = (int)strlen(title); i < 55; i++) putchar('-');
    putchar('\n');
}

/* ════════════════════════════════════════════════════════════════════════════
 * §1  Basic write / read
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_basic_write_read(Account *alice, Account *bob, Account *carol)
{
    section("§1  Basic write / read");

    /*
     * worlds_write() and worlds_read() always target the *current world*.
     * At this point we have not branched, so everything goes into the root
     * world — the permanent, global baseline.
     *
     * account_init() calls account_set() which calls worlds_write() under
     * the hood.  We initialise all three accounts here.
     */
    account_init(alice, "Alice", 500);
    account_init(bob,   "Bob",   300);
    account_init(carol, "Carol", 200);

    printf("Initial state (root world):\n");
    account_print(alice);
    account_print(bob);
    account_print(carol);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §2  Branching and isolation
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_isolation(Account *alice, Account *bob)
{
    section("§2  Branching and isolation");

    /*
     * worlds_sprout() creates a child of the current world.
     * Writes inside the child are completely invisible outside it.
     *
     * WORLDS_WITH(branch) { ... } is syntactic sugar for:
     *
     *   worlds_t *prev = worlds_enter(branch);
     *   { ... }
     *   worlds_leave(prev);
     */
    worlds_t *branch = worlds_sprout();

    WORLDS_WITH(branch) {
        printf("Inside branch (before commit):\n");
        printf("  Alice sees balance=%d (same as root)\n", balance(alice));

        /* Transfer 100 from Alice to Bob — only visible inside `branch`. */
        transfer(alice, bob, 100);

        printf("  After transfer inside branch:\n");
        account_print(alice);
        account_print(bob);
    }

    /*
     * Back in the root world. The branch's writes are not yet here.
     */
    printf("Root world (before commit — branch writes invisible):\n");
    account_print(alice);
    account_print(bob);

    /*
     * worlds_commit() merges the branch into its parent (the root world).
     * It returns a worlds_conflict_t; check has_conflict before proceeding.
     */
    worlds_conflict_t r = worlds_commit(branch);
    if (r.has_conflict) {
        printf("  [unexpected conflict]\n");
    } else {
        printf("Root world (after commit):\n");
        account_print(alice);
        account_print(bob);
    }

    worlds_free(branch);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §3  Conflict detection
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_conflict(Account *alice, Account *bob, Account *carol)
{
    section("§3  Conflict detection");

    /*
     * We create two independent branches off the root world.
     * Both read Alice's balance; then each tries to debit her.
     *
     * The timeline is:
     *
     *   root  alice.balance = 400   (after §2)
     *     ├── branch_a  reads 400, writes 350  (transfer 50 → Bob)
     *     └── branch_b  reads 400, writes 300  (transfer 100 → Carol)
     *
     * branch_a commits first — it wins.
     * branch_b then tries to commit.  It read alice.balance = 400, but the
     * root world now has 350 (changed by branch_a).  That is a conflict.
     */
    worlds_t *branch_a = worlds_sprout();
    worlds_t *branch_b = worlds_sprout();

    WORLDS_WITH(branch_a) {
        printf("Branch A: Alice=%d, transferring 50 → Bob\n", balance(alice));
        transfer(alice, bob, 50);
    }

    WORLDS_WITH(branch_b) {
        printf("Branch B: Alice=%d, transferring 100 → Carol\n", balance(alice));
        transfer(alice, carol, 100);
    }

    /* Commit branch_a — always succeeds because root hasn't changed. */
    worlds_conflict_t ra = worlds_commit(branch_a);
    printf("Branch A commit: conflict=%s\n", ra.has_conflict ? "YES" : "no");

    /* Commit branch_b — conflicts because Alice's balance changed. */
    worlds_conflict_t rb = worlds_commit(branch_b);
    if (rb.has_conflict) {
        /* Identify which field caused the conflict by comparing the slot id. */
        const char *slot_name =
            (rb.slot == WSLOT(Account, balance))  ? "balance"  :
            (rb.slot == WSLOT(Account, tx_count)) ? "tx_count" : "unknown";
        printf("Branch B commit: CONFLICT detected on Alice.%s!\n", slot_name);
        printf("  Branch B read %lu, but root now holds %lu\n",
               (unsigned long)rb.child_value,
               (unsigned long)rb.parent_value);
    }

    printf("Root world (only branch_a was applied):\n");
    account_print(alice);
    account_print(bob);
    account_print(carol);

    worlds_free(branch_a);
    worlds_free(branch_b);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §4  Retry with WORLDS_RETRY
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_retry(Account *alice, Account *carol)
{
    section("§4  Retry with WORLDS_RETRY");

    /*
     * We use WORLDS_RETRY to transfer 50 from Alice to Carol.
     * On each attempt:
     *   1. worlds_reset() clears the branch's read/write logs.
     *   2. The body re-reads current values (so the read log is fresh).
     *   3. worlds_commit() tries to merge.
     *   4. If another thread or branch changed Alice's balance between
     *      the read and the commit, the body is re-run with the updated value.
     *
     * Here we call it once with no concurrent competitors, so it succeeds on
     * the first attempt.  The retry mechanism becomes essential in the
     * multi-threaded scenario shown in §7.
     */
    worlds_t *branch = worlds_sprout();

    printf("Alice before retry-transfer: balance=%d\n", balance(alice));

    WORLDS_RETRY(branch, 5, {
        /*
         * IMPORTANT: every value you base a decision on must be read via
         * worlds_read() *inside* the body — not cached outside it.  The body
         * is re-executed verbatim on each retry, so stale locals would
         * produce incorrect read logs.
         */
        int cur_alice = balance(alice);
        int cur_carol = balance(carol);
        account_set(alice, WSLOT(Account, balance), cur_alice - 50);
        account_set(carol, WSLOT(Account, balance), cur_carol + 50);
        account_set(alice, WSLOT(Account, tx_count), txcount(alice) + 1);
        account_set(carol, WSLOT(Account, tx_count), txcount(carol) + 1);
    });

    printf("After retry-transfer (50 Alice → Carol):\n");
    account_print(alice);
    account_print(carol);

    worlds_free(branch);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §5  Force commit
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_force_commit(Account *bob)
{
    section("§5  Force commit (last-writer-wins)");

    /*
     * worlds_commit_force() skips the conflict check entirely.
     * The child's writes unconditionally overwrite the parent.
     * Use when you deliberately want last-writer-wins semantics —
     * for example, applying a correction or an admin override.
     */
    int old_balance = balance(bob);
    printf("Bob before correction: balance=%d\n", old_balance);

    worlds_t *correction = worlds_sprout();
    worlds_write_in(correction, bob,
                    WSLOT(Account, balance),
                    (uintptr_t)(old_balance + 100));
    worlds_commit_force(correction);
    worlds_free(correction);

    printf("Bob after correction:  balance=%d\n", balance(bob));
}

/* ════════════════════════════════════════════════════════════════════════════
 * §6  Snapshot / audit trail
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_snapshot(Account *alice, Account *bob, Account *carol)
{
    section("§6  Snapshot / audit trail");

    /*
     * worlds_snapshot() makes a deep copy of any world's write log into a
     * fresh world whose parent is the root.  The original world is unaffected.
     *
     * Use cases:
     *   • Audit / history: capture state before a risky operation.
     *   • Rollback: if a sequence of commits goes wrong, re-read from the snap.
     *   • Speculative execution: branch from a known checkpoint.
     */
    worlds_t *before = worlds_snapshot(worlds_root());

    printf("Snapshot taken.  Current balances:\n");
    account_print(alice);
    account_print(bob);
    account_print(carol);

    /* Make a change in the root world. */
    account_set(alice, WSLOT(Account, balance), balance(alice) - 200);
    printf("After subtracting 200 from Alice (root world):\n");
    account_print(alice);

    /* Read from the snapshot — it still holds the old value. */
    uintptr_t snap_bal = 0;
    worlds_read_in(before, alice, WSLOT(Account, balance), &snap_bal);
    printf("Alice's balance in snapshot (old value): %d\n", (int)snap_bal);

    worlds_free(before);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §7  worlds_commit_n with a reset callback
 * ════════════════════════════════════════════════════════════════════════════ */

/*
 * This is the data we pass to the reset callback so it can rebuild the
 * transaction body after each failed attempt.
 */
typedef struct {
    Account *from;
    Account *to;
    int      amount;
} TransferArgs;

/* Called by worlds_commit_n() after each failed commit, once logs are clear. */
static void rebuild_transfer(worlds_t *w, void *userdata)
{
    TransferArgs *args = (TransferArgs *)userdata;

    /* Re-read from the *parent* world so we get the latest committed values. */
    uintptr_t raw_from_bal = 0, raw_to_bal = 0;
    uintptr_t raw_from_tx  = 0, raw_to_tx  = 0;

    worlds_read_in(w->parent, args->from, WSLOT(Account, balance),  &raw_from_bal);
    worlds_read_in(w->parent, args->from, WSLOT(Account, tx_count), &raw_from_tx);
    worlds_read_in(w->parent, args->to,   WSLOT(Account, balance),  &raw_to_bal);
    worlds_read_in(w->parent, args->to,   WSLOT(Account, tx_count), &raw_to_tx);

    int new_from_bal = (int)raw_from_bal - args->amount;
    int new_to_bal   = (int)raw_to_bal   + args->amount;

    /* Build fresh read + write entries in the (now-cleared) world. */
    worlds_write_in(w, args->from, WSLOT(Account, balance),  (uintptr_t)(new_from_bal));
    worlds_write_in(w, args->from, WSLOT(Account, tx_count), (uintptr_t)((int)raw_from_tx + 1));
    worlds_write_in(w, args->to,   WSLOT(Account, balance),  (uintptr_t)(new_to_bal));
    worlds_write_in(w, args->to,   WSLOT(Account, tx_count), (uintptr_t)((int)raw_to_tx  + 1));

    printf("  [reset callback] rebuilt transfer: from=%d to=%d\n",
           new_from_bal, new_to_bal);
}

static void demo_commit_n(Account *bob, Account *carol)
{
    section("§7  worlds_commit_n with a reset callback");

    /*
     * worlds_commit_n() is the lower-level counterpart to WORLDS_RETRY.
     * You prepare the world yourself, then call commit_n.  On each conflict:
     *   1. worlds_reset() clears the logs.
     *   2. Your fn_reset() is called so you can rebuild the transaction.
     *   3. commit is attempted again.
     *
     * This gives you full control over what "retry" means — useful when
     * re-executing the original code block is not convenient.
     */
    TransferArgs args = { bob, carol, 30 };

    worlds_t *w = worlds_sprout();

    /* Prime the world with the first attempt. */
    rebuild_transfer(w, &args);

    printf("Bob before commit_n transfer: balance=%d\n", balance(bob));

    worlds_conflict_t r = worlds_commit_n(w, 5, rebuild_transfer, &args);
    printf("commit_n result: conflict=%s\n", r.has_conflict ? "YES" : "no");

    printf("After transfer (30 Bob → Carol):\n");
    account_print(bob);
    account_print(carol);

    worlds_free(w);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §8  Introspection helpers
 * ════════════════════════════════════════════════════════════════════════════ */

static void print_entry(void *obj, uintptr_t slot, uintptr_t value, void *ud)
{
    (void)ud;
    printf("  object=%p  slot=%-4lu  value=%lu\n",
           obj, (unsigned long)slot, (unsigned long)value);
}

static void demo_introspection(Account *alice, Account *bob)
{
    section("§8  Introspection");

    worlds_t *w = worlds_sprout();

    WORLDS_WITH(w) {
        /* Two writes, one read-through (causes alice.balance to be cached
           in the read log because it comes from an ancestor world). */
        transfer(alice, bob, 10);
    }

    printf("World depth:  %zu\n", worlds_depth(w));
    printf("Write count:  %zu\n", worlds_write_count(w));
    printf("Read  count:  %zu\n", worlds_read_count(w));

    printf("has_write alice.balance: %s\n",
           worlds_has_write(w, alice, WSLOT(Account, balance)) ? "yes" : "no");
    printf("has_read  alice.balance: %s\n",
           worlds_has_read(w, alice, WSLOT(Account, balance)) ? "yes" : "no");

    printf("Write log entries:\n");
    worlds_foreach_write(w, print_entry, NULL);

    printf("Read log entries:\n");
    worlds_foreach_read(w, print_entry, NULL);

    /* worlds_peek inspects only the world's own write log — no chain walk. */
    uintptr_t peeked = 0;
    bool found = worlds_peek(w, alice, WSLOT(Account, balance), &peeked);
    printf("worlds_peek alice.balance: found=%s value=%d\n",
           found ? "yes" : "no", (int)peeked);

    worlds_free(w);
}

/* ════════════════════════════════════════════════════════════════════════════
 * §9  WORLDS_DEFER_FREE
 * ════════════════════════════════════════════════════════════════════════════ */

static void demo_defer_free(Account *alice, Account *bob)
{
    section("S9  WORLDS_DEFER_FREE (auto cleanup)");

    /*
     * WORLDS_DEFER_FREE(w) { ... } is a portable block-wrapper macro.
     * It expands to a for-loop whose increment calls worlds_free(w) after
     * the body runs, using only standard C99 — no compiler extensions needed.
     *
     * The for-loop guard (_wdf_N) is named with __LINE__ so multiple uses
     * in the same function do not collide.
     *
     * Constraint (same as WORLDS_WITH): worlds_free is called only when
     * control falls off the bottom of the block naturally.  If you need to
     * exit early via return/break/goto, call worlds_free() yourself.
     *
     * Two WORLDS_DEFER_FREE blocks here to show __LINE__ uniqueness works
     * correctly when the macro is used more than once in the same function.
     */
    worlds_t *branch = worlds_sprout();
    WORLDS_DEFER_FREE(branch) {
        WORLDS_WITH(branch) {
            transfer(alice, bob, 5);
            printf("Inside deferred branch: Alice=%d, Bob=%d\n",
                   balance(alice), balance(bob));
        }
        worlds_commit(branch);
        printf("After first deferred commit: Alice=%d, Bob=%d\n",
               balance(alice), balance(bob));
    } /* worlds_free(branch) runs here — inside the for-loop increment */

    /* Second use in the same function — __LINE__ gives a different guard
       name so the two loops do not interfere. */
    worlds_t *branch2 = worlds_sprout();
    WORLDS_DEFER_FREE(branch2) {
        WORLDS_WITH(branch2) {
            transfer(bob, alice, 3);
            printf("Inside second deferred branch: Alice=%d, Bob=%d\n",
                   balance(alice), balance(bob));
        }
        worlds_commit(branch2);
        printf("After second deferred commit: Alice=%d, Bob=%d\n",
               balance(alice), balance(bob));
    } /* worlds_free(branch2) runs here */

    printf("Both blocks exited; both worlds freed automatically.\n");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Summary
 * ════════════════════════════════════════════════════════════════════════════ */

static void print_summary(Account *alice, Account *bob, Account *carol)
{
    section("Final state (root world)");
    account_print(alice);
    account_print(bob);
    account_print(carol);
}

/* ════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /*
     * Call worlds_root() once from the main thread before doing anything else.
     * It initialises the global root world.  In C11 the local-static init is
     * thread-safe; in C99 it must happen before any worker threads are spawned.
     */
    worlds_root();

    /*
     * Our three accounts live on the stack.  They are plain C structs; the
     * worlds library tracks their fields by (pointer, slot-id) pairs without
     * any intrusive modification to the struct layout.
     */
    Account alice, bob, carol;

    demo_basic_write_read(&alice, &bob, &carol);
    demo_isolation(&alice, &bob);
    demo_conflict(&alice, &bob, &carol);
    demo_retry(&alice, &carol);
    demo_force_commit(&bob);
    demo_snapshot(&alice, &bob, &carol);
    demo_commit_n(&bob, &carol);
    demo_introspection(&alice, &bob);
    demo_defer_free(&alice, &bob);
    print_summary(&alice, &bob, &carol);

    return 0;
}
