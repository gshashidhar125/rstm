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
 *  LLT Implementation
 *
 *    This STM very closely resembles the GV1 variant of TL2.  That is, it uses
 *    orecs and lazy acquire.  Its clock requires everyone to increment it to
 *    commit writes, but this allows for read-set validation to be skipped at
 *    commit time.  Most importantly, there is no in-flight validation: if a
 *    timestamp is greater than when the transaction sampled the clock at begin
 *    time, the transaction aborts.
 */

#include <iostream>
#include <setjmp.h>
#include "byte-logging.hpp"
#include "tmabi-weak.hpp"               // the weak stm interface
#include "foreach.hpp"                  // FOREACH macro
#include "MiniVector.hpp"
#include "metadata.hpp"
#include "WriteSet.hpp"
#include "WBMMPolicy.hpp"
#include "tx.hpp"
#include "adaptivity.hpp"
#include "tm_alloc.hpp"
#include "libitm.h"
#include "inst.hpp"                     // read<>/write<>

using namespace stm;

/** For querying to get the current algorithm name */
const char* alg_tm_getalgname() {
    return "LLT";
}

/** Abort and roll back the transaction (e.g., on conflict). */
void alg_tm_rollback(TX* tx) {
    ++tx->aborts;

    // release the locks and restore version numbers
    FOREACH (OrecList, i, tx->locks) {
        (*i)->v.all = (*i)->p;
    }

    // undo memory operations, reset lists
    tx->r_orecs.reset();
    tx->writes.reset();
    tx->locks.reset();
    tx->allocator.onTxAbort();
}

/*** The only metadata we need is a single global padded lock ***/
static pad_word_t timestamp = {0};

/** LLT begin: only called for outermost transactions. */
uint32_t alg_tm_begin(uint32_t, TX* tx)
{
    tx->allocator.onTxBegin();
    // get a start time
    tx->start_time = timestamp.val;

    return a_runInstrumentedCode;
}

/** LLT validation */
static NOINLINE void validate(TX* tx)
{
    // validate
    FOREACH (OrecList, i, tx->r_orecs) {
        uintptr_t ivt = (*i)->v.all;
        // if unlocked and newer than start time, abort
        if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
            _ITM_abortTransaction(TMConflict);
    }
}

/** LLT commit (read-only): */
void alg_tm_end()
{
    TX* tx = Self;
    if (--tx->nesting_depth)
        return;

    if (!tx->writes.size()) {
        // read-only, so just reset lists
        tx->r_orecs.reset();
        tx->allocator.onTxCommit();
        ++tx->commits_ro;
        return;
    }

    // acquire locks
    FOREACH (WriteSet, i, tx->writes) {
        // get orec, read its version#
        orec_t* o = get_orec(i->addr);
        uintptr_t ivt = o->v.all;

        // lock all orecs, unless already locked
        if (ivt <= tx->start_time) {
            // abort if cannot acquire
            if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                _ITM_abortTransaction(TMConflict);
            // save old version to o->p, remember that we hold the lock
            o->p = ivt;
            tx->locks.insert(o);
        }
        // else if we don't hold the lock abort
        else if (ivt != tx->my_lock.all) {
            _ITM_abortTransaction(TMConflict);
        }
    }

    // increment the global timestamp since we have writes
    uintptr_t end_time = 1 + faiptr(&timestamp.val);

    // skip validation if nobody else committed
    if (end_time != (tx->start_time + 1))
        validate(tx);

    // run the redo log
    tx->writes.writeback();

    // release locks
    CFENCE;
    FOREACH (OrecList, i, tx->locks)
    (*i)->v.all = end_time;

    // clean-up
    tx->r_orecs.reset();
    tx->writes.reset();
    tx->locks.reset();

    tx->allocator.onTxCommit();
    ++tx->commits_rw;
}

/**
 *  LLT read (read-only transaction)
 *
 *    We use "check twice" timestamps in LLT
 */
static inline void* alg_tm_read_aligned_word(void** addr, TX* tx) {
    // get the orec addr
    orec_t* o = get_orec(addr);

    // read orec, then val, then orec
    uintptr_t ivt = o->v.all;
    CFENCE;
    void* tmp = *addr;
    CFENCE;
    uintptr_t ivt2 = o->v.all;

    // if orec is too new, or we didn't see a consistent version, abort
    if ((ivt > tx->start_time) || (ivt != ivt2))
        _ITM_abortTransaction(TMConflict);

    // log orec, return the value
    tx->r_orecs.insert(o);
    return tmp;
}

/** LLT write */
static inline void ALG_TM_WRITE_WORD(void** addr, void* val, TX* tx, uintptr_t mask)
{
    tx->writes.insert(WriteSetEntry(REDO_LOG_ENTRY(addr, val, mask)));
}

void* alg_tm_read(void** addr) {
    return inst::read<void*, inst::NoFilter, inst::WordlogRAW, true>(addr);
}

void alg_tm_write(void** addr, void* val) {
    ALG_TM_WRITE_WORD(addr, val, Self, ~0);
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
 *  Register the TM for adaptivity and for use as a standalone library
 */
REGISTER_TM_FOR_ADAPTIVITY(LLT);