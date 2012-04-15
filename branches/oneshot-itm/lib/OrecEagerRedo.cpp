/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  OrecEagerRedo Implementation
 *
 *    This code is very similar to the TinySTM-writeback algorithm.  It can
 *    also be thought of as OrecEager with redo logs instead of undo logs.
 *    Note, though, that it uses timestamps as in Wang's CGO 2007 paper, so
 *    we always validate at commit time but we don't have to check orecs
 *    twice during each read.
 */

#include <iostream>
#include "byte-logging.hpp"
#include "tmabi-weak.hpp"
#include "foreach.hpp"
#include "MiniVector.hpp"
#include "metadata.hpp"
#include "WBMMPolicy.hpp"
#include "tx.hpp"
#include "adaptivity.hpp"
#include "tm_alloc.hpp"
#include "libitm.h"
#include "inst.hpp"

using namespace stm;

/**
 *  For querying to get the current algorithm name
 */
const char* alg_tm_getalgname() {
    return "OrecEagerRedo";
}

/**
 *  OrecEagerRedo unwinder:
 *
 *    To unwind, we must release locks, but we don't have an undo log to run.
 */
void alg_tm_rollback(TX* tx)
{
    ++tx->aborts;

    // release the locks and restore version numbers
    FOREACH (OrecList, i, tx->locks) {
        (*i)->v.all = (*i)->p;
    }

    // undo memory operations, reset lists
    tx->r_orecs.reset();
    tx->writes.clear();
    tx->locks.reset();
    tx->allocator.onTxAbort();
}

/*** The only metadata we need is a single global padded lock ***/
static pad_word_t timestamp = {0};

/**
 *  OrecEagerRedo begin:
 *
 *    Standard begin: just get a start time, only called for outermost
 *    transactions.
 */
uint32_t alg_tm_begin(uint32_t, TX* tx)
{
    tx->allocator.onTxBegin();
    tx->start_time = timestamp.val;
    return a_runInstrumentedCode;
}

/**
 *  OrecEagerRedo validation
 *
 *    validate the read set by making sure that all orecs that we've read have
 *    timestamps older than our start time, unless we locked those orecs.
 */
static NOINLINE void validate(TX* tx)
{
    FOREACH (OrecList, i, tx->r_orecs) {
        // read this orec
        uintptr_t ivt = (*i)->v.all;
        // if unlocked and newer than start time, abort
        if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
            _ITM_abortTransaction(TMConflict);
    }
}

/**
 *  OrecEagerRedo commit (read-only):
 *
 *    Standard commit: we hold no locks, and we're valid, so just clean up
 */
void alg_tm_end()
{
    TX* tx = Self;
    if (--tx->nesting_depth)
        return;

    if (!tx->writes.size()) {
        tx->r_orecs.reset();
        tx->allocator.onTxCommit();
        ++tx->commits_ro;
        return;
    }

    // note: we're using timestamps in the same manner as
    // OrecLazy... without the single-thread optimization

    // we have all locks, so validate
    FOREACH (OrecList, i, tx->r_orecs) {
        // read this orec
        uintptr_t ivt = (*i)->v.all;
        // if unlocked and newer than start time, abort
        if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
            _ITM_abortTransaction(TMConflict);
    }

    // run the redo log
    tx->writes.redo();

    // we're a writer, so increment the global timestamp
    uintptr_t end_time = 1 + faiptr(&timestamp.val);

    // release locks
    FOREACH (OrecList, i, tx->locks) {
        (*i)->v.all = end_time;
    }

    // clean up
    tx->r_orecs.reset();
    tx->writes.clear();
    tx->locks.reset();
    tx->allocator.onTxCommit();
    ++tx->commits_rw;
}

/** OrecEagerRedo read */
static inline void* alg_tm_read_aligned_word(void** addr, TX* tx, uintptr_t) {
    // get the orec addr
    orec_t* o = get_orec(addr);
    while (true) {
        // read the location
        void* tmp = *addr;
        CFENCE;
        // read orec
        id_version_t ivt; ivt.all = o->v.all;

        // common case: new read to uncontended location
        if (ivt.all <= tx->start_time) {
            tx->r_orecs.insert(o);
            return tmp;
        }

        // next best: locked by me
        // [TODO] This needs to deal with byte logging. Both the RAW and the
        // *addr are dangerous.
        if (ivt.all == tx->my_lock.all) {
            // check the log for a RAW hazard
            const WriteSet::Word* const found = tx->writes.find(addr);
            return (found->mask()) ? found->value() : *addr;
        }

        // abort if locked by other
        if (ivt.fields.lock)
            _ITM_abortTransaction(TMConflict);

        // scale timestamp if ivt is too new
        uintptr_t newts = timestamp.val;
        validate(tx);
        tx->start_time = newts;
    }
}

static inline void* alg_tm_read_aligned_word_ro(void** addr, TX* tx,
                                                uintptr_t mask)
{
    return alg_tm_read_aligned_word(addr, tx, mask);
}

/**
 *  OrecEagerRedo write
 */
static inline void alg_tm_write_aligned_word(void** addr, void* val, TX* tx, uintptr_t mask)
{
    // add to redo log
    tx->writes.insert(addr, WriteSet::Word(val, mask));

    // get the orec addr
    orec_t* o = get_orec(addr);
    while (true) {
        // read the orec version number
        id_version_t ivt;
        ivt.all = o->v.all;

        // common case: uncontended location... lock it
        if (ivt.all <= tx->start_time) {
            if (!bcasptr(&o->v.all, ivt.all, tx->my_lock.all))
                _ITM_abortTransaction(TMConflict);

            // save old, log lock, write, return
            o->p = ivt.all;
            tx->locks.insert(o);
            return;
        }

        // next best: already have the lock
        if (ivt.all == tx->my_lock.all)
            return;

        // fail if lock held
        if (ivt.fields.lock)
            _ITM_abortTransaction(TMConflict);

        // unlocked but too new... scale forward and try again
        uintptr_t newts = timestamp.val;
        validate(tx);
        tx->start_time = newts;
    }
}

void* alg_tm_read(void** addr) {
    return stm::inst::read<void*,                 //
                           stm::inst::NoFilter,   // don't pre-filter accesses
                           stm::inst::NoRAW,      // RAW is done internally
                           stm::inst::NoReadOnly, // no separate read-only code
                           true                   // force align all accesses
                           >(addr);

}

void alg_tm_write(void** addr, void* val) {
    alg_tm_write_aligned_word(addr, val, Self, ~0);
}

bool alg_tm_is_irrevocable(TX*) {
    assert(false && "Unimplemented");
    return false;
}

void alg_tm_become_irrevocable(_ITM_transactionState) {
    assert(false && "Unimplemented");
    return;
}

/**
 * Register the TM for adaptivity and for use as a standalone library
 */
REGISTER_TM_FOR_ADAPTIVITY(OrecEagerRedo)
