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
 *  OrecLazy Implementation:
 *
 *    This STM is similar to the commit-time locking variant of TinySTM.  It
 *    also resembles the "patient" STM published by Spear et al. at PPoPP 2009.
 *    The key difference deals with the way timestamps are managed.  This code
 *    uses the manner of timestamps described by Wang et al. in their CGO 2007
 *    paper.  More details can be found in the OrecEager implementation.
 */

#include <iostream>
#include <setjmp.h>
#include "MiniVector.hpp"
#include "metadata.hpp"
#include "WriteSet.hpp"
#include "WBMMPolicy.hpp"
#include "Macros.hpp"
#include "locks.hpp"
#include "tx.hpp"
#include "libitm.h"

using namespace stm;

/**
 *  OrecLazy unwinder:
 *
 *    To unwind, we must release locks, but we don't have an undo log to run.
 */
template <class CM>
static checkpoint_t* rollback(TX* tx)
{
    ++tx->aborts;

    // release the locks and restore version numbers
    foreach (OrecList, i, tx->locks)
    (*i)->v.all = (*i)->p;

    // undo memory operations, reset lists
    CM::onAbort(tx);
    tx->r_orecs.reset();
    tx->writes.reset();
    tx->locks.reset();
    tx->allocator.onTxAbort();
    return &tx->checkpoint;
}

/*** The only metadata we need is a single global padded lock ***/
static pad_word_t timestamp = {0};

/**
 *  OrecLazy begin:
 *
 *    Standard begin: just get a start time. only called for outermost
 *    transactions.
 */
template <class CM>
static uint32_t tm_begin(uint32_t, TX* tx)
{
    CM::onBegin(tx);
    tx->allocator.onTxBegin();
    tx->start_time = timestamp.val;
    return a_runInstrumentedCode;
}

/**
 *  OrecLazy validation
 *
 *    validate the read set by making sure that all orecs that we've read have
 *    timestamps older than our start time, unless we locked those orecs.
 */
static NOINLINE void validate(TX* tx)
{
    foreach (OrecList, i, tx->r_orecs)
    // abort if orec locked, or if unlocked but timestamp too new
    if ((*i)->v.all > tx->start_time)
        tm_abort(tx);
}

/**
 *  OrecLazy commit (read-only):
 *
 *    Standard commit: we hold no locks, and we're valid, so just clean up
 */
template <class CM>
static void tm_end()
{
    TX* tx = Self;
    if (--tx->nesting_depth)
        return;

    if (!tx->writes.size()) {
        tx->r_orecs.reset();
        tx->allocator.onTxCommit();
        ++tx->commits_ro;
        CM::onCommit(tx);
        return;
    }

    // note: we're using timestamps in the same manner as
    // OrecLazy... without the single-thread optimization

    // acquire locks
    foreach (WriteSet, i, tx->writes) {
        // get orec, read its version#
        orec_t* o = get_orec(i->addr);
        uintptr_t ivt = o->v.all;

        // lock all orecs, unless already locked
        if (ivt <= tx->start_time) {
            // abort if cannot acquire
            if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                tm_abort(tx);
            // save old version to o->p, remember that we hold the lock
            o->p = ivt;
            tx->locks.insert(o);
        }
        // else if we don't hold the lock abort
        else if (ivt != tx->my_lock.all) {
            tm_abort(tx);
        }
    }

    // validate
    foreach (OrecList, i, tx->r_orecs) {
        uintptr_t ivt = (*i)->v.all;
        // if unlocked and newer than start time, abort
        if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
            tm_abort(tx);
    }

    // run the redo log
    tx->writes.writeback();

    // increment the global timestamp, release locks
    uintptr_t end_time = 1 + faiptr(&timestamp.val);
    foreach (OrecList, i, tx->locks)
    (*i)->v.all = end_time;

    // clean up
    CM::onCommit(tx);
    tx->r_orecs.reset();
    tx->writes.reset();
    tx->locks.reset();
    tx->allocator.onTxCommit();
    ++tx->commits_rw;
}

/**
 *  OrecLazy read
 */
static TM_FASTCALL void* tm_read(void** addr)
{
    TX* tx = Self;

    if (tx->writes.size()) {
        // check the log for a RAW hazard, we expect to miss
        WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
        bool found = tx->writes.find(log);
        if (found)
            return log.val;
    }

    // get the orec addr
    orec_t* o = get_orec(addr);
    while (true) {
        // read the location
        void* tmp = *addr;
        CFENCE;
        // read orec
        id_version_t ivt;
        ivt.all = o->v.all;

        // common case: new read to uncontended location
        if (ivt.all <= tx->start_time) {
            tx->r_orecs.insert(o);
            return tmp;
        }

        // if lock held, spin and retry
        if (ivt.fields.lock) {
            spin64();
            continue;
        }

        // scale timestamp if ivt is too new
        uintptr_t newts = timestamp.val;
        validate(tx);
        tx->start_time = newts;
    }
}

/**
 *  OrecLazy write
 */
static TM_FASTCALL void tm_write(void** addr, void* val)
{
    TX* tx = Self;
    // add to redo log
    tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));

}
