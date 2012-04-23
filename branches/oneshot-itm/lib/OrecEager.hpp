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
 *  OrecEager Implementation:
 *
 *    This STM is similar to LSA/TinySTM and to the algorithm published by
 *    Wang et al. at CGO 2007.  The algorithm uses a table of orecs, direct
 *    update, encounter time locking, and undo logs.
 *
 *    The principal difference is in how OrecEager handles the modification
 *    of orecs when a transaction aborts.  In Wang's algorithm, a thread at
 *    commit time will first validate, then increment the counter.  This
 *    allows for threads to skip prevalidation of orecs in their read
 *    functions... however, it necessitates good CM, because on abort, a
 *    transaction must run its undo log, then get a new timestamp, and then
 *    release all orecs at that new time.  In essence, the aborted
 *    transaction does "silent stores", and these stores can cause other
 *    transactions to abort.
 *
 *    In LSA/TinySTM, each orec includes an "incarnation number" in the low
 *    bits.  When a transaction aborts, it runs its undo log, then it
 *    releases all locks and bumps the incarnation number.  If this results
 *    in incarnation number wraparound, then the abort function must
 *    increment the timestamp in the orec being released.  If this timestamp
 *    is larger than the current max timestamp, the aborting transaction must
 *    also bump the timestamp.  This approach has a lot of corner cases, but
 *    it allows for the abort-on-conflict contention manager.
 *
 *    In our code, we skip the incarnation numbers, and simply say that when
 *    releasing locks after undo, we increment each, and we keep track of the
 *    max value written.  If the value is greater than the timestamp, then at
 *    the end of the abort code, we increment the timestamp.  A few simple
 *    invariants about time ensure correctness.
 */

#include <iostream>
#include "byte-logging.hpp"
#include "tmabi-weak.hpp"
#include "foreach.hpp"
#include "inst3.hpp"                    // read<>/write<>, etc
#include "MiniVector.hpp"
#include "metadata.hpp"
#include "WBMMPolicy.hpp"
#include "tx.hpp"
#include "libitm.h"

using namespace stm;

/**
 *  OrecEager rollback:
 *
 *    Run the redo log, possibly bump timestamp
 */

/*** The only metadata we need is a single global padded lock ***/
static pad_word_t timestamp = {0};

template <class CM>
static void alg_tm_rollback(TX* tx)
{
    ++tx->aborts;

    tx->undo_log.undo();

    // release the locks and bump version numbers by one... track the highest
    // version number we write, in case it is greater than timestamp.val
    uintptr_t max = 0;
    FOREACH (OrecList, j, tx->locks) {
        uintptr_t newver = (*j)->p + 1;
        (*j)->v.all = newver;
        max = (newver > max) ? newver : max;
    }
    // if we bumped a version number to higher than the timestamp, we need to
    // increment the timestamp to preserve the invariant that the timestamp
    // val is >= all orecs' values when unlocked
    uintptr_t ts = timestamp.val;
    if (max > ts)
        casptr(&timestamp.val, ts, (ts+1)); // assumes no transient failures

    // reset all lists
    CM::onAbort(tx);
    tx->r_orecs.reset();
    tx->undo_log.reset();
    tx->locks.reset();

    tx->allocator.onTxAbort();
}

/** only called for outermost transactions. */
template <class CM>
static uint32_t TM_FASTCALL alg_tm_begin(uint32_t, TX* tx)
{
    CM::onBegin(tx);
    // sample the timestamp and prepare local structures
    tx->allocator.onTxBegin();
    tx->start_time = timestamp.val;

    return a_runInstrumentedCode;
}

static NOINLINE void validate_commit(TX* tx)
{
    FOREACH (OrecList, i, tx->r_orecs) {
        // abort unless orec older than start or owned by me
        uintptr_t ivt = (*i)->v.all;
        if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
            _ITM_abortTransaction(TMConflict);
    }
}

/**
 *  OrecEager validation:
 *
 *    Make sure that all orecs that we've read have timestamps older than our
 *    start time, unless we locked those orecs.  If we locked the orec, we
 *    did so when the time was smaller than our start time, so we're sure to
 *    be OK.
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
 *  OrecEager commit:
 *
 *    read-only transactions do no work
 *
 *    writers must increment the timestamp, maybe validate, and then release
 *    locks
 */
template <class CM>
static void alg_tm_end()
{
    TX* tx = Self;
    if (--tx->nesting_depth)
        return;

    // use the lockset size to identify if tx is read-only
    if (!tx->locks.size()) {
        tx->r_orecs.reset();
        tx->allocator.onTxCommit();
        ++tx->commits_ro;
        CM::onCommit(tx);
        return;
    }

    // increment the global timestamp
    uintptr_t end_time = 1 + faiptr(&timestamp.val);

    // skip validation if nobody else committed since my last validation
    if (end_time != (tx->start_time + 1))
        validate_commit(tx);

    // release locks
    FOREACH (OrecList, i, tx->locks) {
        (*i)->v.all = end_time;
    }

    // reset lock list and undo log
    CM::onCommit(tx);
    tx->locks.reset();
    tx->undo_log.reset();
    tx->r_orecs.reset();
    tx->allocator.onTxCommit();
    ++tx->commits_rw;
}

namespace {
/**
 *  OrecEager read:
 *
 *    Must check orec twice, and may need to validate
 */
  struct Read {
      void* operator()(void** addr, TX* tx, uintptr_t) const {
          // get the orec addr, then start loop to read a consistent value
          orec_t* o = get_orec(addr);
          while (true) {
              // read the orec BEFORE we read anything else
              id_version_t ivt;
              ivt.all = o->v.all;
              CFENCE;

              // read the location
              void* tmp = *addr;

              // best case: I locked it already
              if (ivt.all == tx->my_lock.all)
                  return tmp;

              // re-read orec AFTER reading value
              CFENCE;
              uintptr_t ivt2 = o->v.all;

              // common case: new read to an unlocked, old location
              if ((ivt.all == ivt2) && (ivt.all <= tx->start_time)) {
                  tx->r_orecs.insert(o);
                  return tmp;
              }

              // abort if locked
              if (__builtin_expect(ivt.fields.lock, 0))
                  _ITM_abortTransaction(TMConflict);

              // scale timestamp if ivt is too new, then try again
              uintptr_t newts = timestamp.val;
              validate(tx);
              tx->start_time = newts;
          }
      }
  };

  template <typename WordType>
  struct Write {
      void operator()(void** addr, void* val, TX* tx, uintptr_t mask) const {
          // get the orec addr, then enter loop to get lock from a consistent
          // state
          orec_t* o = get_orec(addr);
          while (true) {
              // read the orec version number
              id_version_t ivt;
              ivt.all = o->v.all;

              // common case: uncontended location... try to lock it, abort on
              //              fail
              if (ivt.all <= tx->start_time) {
                  if (!bcasptr(&o->v.all, ivt.all, tx->my_lock.all))
                      _ITM_abortTransaction(TMConflict);

                  // save old value, log lock, do the write, and return
                  o->p = ivt.all;
                  tx->locks.insert(o);
                  tx->undo_log.insert(addr, *addr, mask);
                  WordType::Write(addr, val, mask);
                  return;
              }

              // next best: I already have the lock... must log old value,
              //            because many locations hash to the same orec.  The
              //            lock does not mean I have undo logged *this*
              //            location
              if (ivt.all == tx->my_lock.all) {
                  tx->undo_log.insert(addr, *addr, mask);
                  WordType::Write(addr, val, mask);
                  return;
              }

              // fail if lock held by someone else
              if (ivt.fields.lock)
                  _ITM_abortTransaction(TMConflict);

              // unlocked but too new... scale forward and try again
              uintptr_t newts = timestamp.val;
              validate(tx);
              tx->start_time = newts;
          }
      }
  };

  template <typename T>
  struct Inst {
      typedef GenericInst<T, true, Word,
                          CheckWritesetForReadOnly,
                          NoFilter, Read, NullType,
                          NoFilter, Write<Word>, NullType> RSTM;
      typedef GenericInst<T, false, LoggingWordType,
                          CheckWritesetForReadOnly,
                          FullFilter, Read, NullType,
                          FullFilter, Write<LoggingWordType>, NullType> ITM;
  };
}

void* alg_tm_read(void** addr) {
    return Inst<void*>::RSTM::Read(addr);
}

void alg_tm_write(void** addr, void* val) {
    Inst<void*>::RSTM::Write(addr, val);
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
 *  Instantiate our read template for all of the read types, and add weak
 *  aliases for the LIBITM symbols to them.
 *
 *  TODO: We can't make weak aliases without mangling the symbol names, but
 *        this is non-trivial for the instrumentation templates. For now, we
 *        just inline the read templates into weak versions of the library. We
 *        could use gcc's asm() exetension to instantiate the template with a
 *        reasonable name...?
 */
#define RSTM_LIBITM_READ(SYMBOL, CALLING_CONVENTION, TYPE)              \
    TYPE CALLING_CONVENTION __attribute__((weak)) SYMBOL(TYPE* addr) {  \
        return Inst<TYPE>::ITM::Read(addr);                             \
    }

#define RSTM_LIBITM_WRITE(SYMBOL, CALLING_CONVENTION, TYPE)             \
    void CALLING_CONVENTION __attribute__((weak))                       \
        SYMBOL(TYPE* addr, TYPE val) {                                  \
        Inst<TYPE>::ITM::Write(addr, val);                              \
    }

#include "libitm-dtfns.def"

#undef RSTM_LIBITM_WRITE
#undef RSTM_LIBITM_READ
