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
 *  Cohorts Implementation
 *
 *  Cohorts has 4 stages. 1) Nobody is running. If anyone starts,
 *  goes to 2) Everybody is running. If anyone is ready to commit,
 *  goes to 3) Every rw tx gets an order, from now on, no one is
 *  allowed to start a tx anymore. When everyone in this cohort is
 *  ready to commit, goes to stage 4)Commit phase. Everyone commits
 *  in an order that given in stage 3. When the last one finishes
 *  its commit, it goes to stage 1. Now tx is allowed to start again.
 */

#include "../profiling.hpp"
#include "algs.hpp"
#include "RedoRAWUtils.hpp"

// define atomic operations
#define CAS __sync_val_compare_and_swap
#define ADD __sync_fetch_and_add
#define SUB __sync_fetch_and_sub

using stm::TxThread;
using stm::threads;
using stm::last_complete;
using stm::timestamp;
using stm::timestamp_max;
using stm::WriteSet;
using stm::OrecList;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::orec_t;
using stm::get_orec;

using stm::started;
using stm::cpending;
using stm::committed;
using stm::last_order;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  struct Cohorts {
      static TM_FASTCALL bool begin(TxThread*);
      static TM_FASTCALL void* read_ro(STM_READ_SIG(,,));
      static TM_FASTCALL void* read_rw(STM_READ_SIG(,,));
      static TM_FASTCALL void write_ro(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void write_rw(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void commit_ro(TxThread* tx);
      static TM_FASTCALL void commit_rw(TxThread* tx);

      static stm::scope_t* rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
      static NOINLINE void validate(TxThread* tx, uintptr_t finish_cache);
      static NOINLINE void TxAbortWrapper(TxThread* tx);
  };

  /**
   *  Cohorts begin:
   *  Cohorts has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  bool
  Cohorts::begin(TxThread* tx)
  {
    S1:
      // wait until everyone is committed
      while (cpending != committed){
          // check if an adaptivity action is underway
          if (TxThread::tmbegin != begin){
              tx->tmabort(tx);
          }
      }

      // before tx begins, increase total number of tx
      ADD(&started, 1);

      // [NB] we must double check no one is ready to commit yet!
      if (cpending > committed){
          SUB(&started, 1);
          goto S1;
      }

      tx->allocator.onTxBegin();

      // get time of last finished txn
      tx->ts_cache = last_complete.val;

      return false;
  }

  /**
   *  Cohorts commit (read-only):
   */
  void
  Cohorts::commit_ro(TxThread* tx)
  {
      // decrease total number of tx started
      SUB(&started, 1);

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  Cohorts commit (writing context):
   *
   *  RW commit is operated in turns. Transactions will be allowed to commit
   *  in an order which is given at the beginning of commit.
   */
  void
  Cohorts::commit_rw(TxThread* tx)
  {
      // increase # of tx waiting to commit
      ADD(&cpending ,1);

      // get an order
      tx->order = 1 + faiptr(&timestamp.val);

      // Wait until it is our turn to commit
      while (last_complete.val != (uintptr_t)(tx->order - 1)) {
          if (TxThread::tmbegin != begin)
              TxAbortWrapper(tx);
      }

      // Wait until every tx is ready to commit
      while (cpending < started)
          if (TxThread::tmbegin != begin)
              TxAbortWrapper(tx);

      // [NB] The first tx to commit in one cohort needs no validation
      // The first one in a cohort, validate read
      if (tx->order != last_order)
          validate(tx, last_complete.val);
      // The last one in a cohort, update last_order.
      if (started - tx->order == 0)
          last_order = tx->order + 1;

      // mark every location in the write set, and do write-back
      foreach (WriteSet, i, tx->writes) {
          // get orec
          orec_t* o = get_orec(i->addr);
          // mark orec
          o->v.all = tx->order;
          CFENCE; // WBW
          // write-back
          *i->addr = i->val;
      }

      CFENCE;
      // mark self as done
      last_complete.val = tx->order;

      // increase total number of committed tx
      ADD(&committed, 1);

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  Cohorts read (read-only transaction)
   *  Standard orec read function.
   */
  void*
  Cohorts::read_ro(STM_READ_SIG(tx,addr,))
  {
      // log orec
      tx->r_orecs.insert( (orec_t*) get_orec(addr) );
      return *addr;
  }

  /**
   *  Cohorts read (writing transaction)
   */
  void*
  Cohorts::read_rw(STM_READ_SIG(tx,addr,mask))
  {
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // log orec
      tx->r_orecs.insert( (orec_t*) get_orec(addr) );

      void* tmp = *addr;
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      return tmp;
  }

  /**
   *  Cohorts write (read-only context)
   */
  void
  Cohorts::write_ro(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  Cohorts write (writing context)
   */
  void
  Cohorts::write_rw(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  Cohorts unwinder:
   */
  stm::scope_t*
  Cohorts::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();

      return PostRollback(tx);
  }

  /**
   *  Cohorts in-flight irrevocability:
   */
  bool
  Cohorts::irrevoc(TxThread*)
  {
      UNRECOVERABLE("Cohorts Irrevocability not yet supported");
      return false;
  }

  /**
   *  Cohorts validation for commit: check that all reads are valid
   */
  void
  Cohorts::validate(TxThread* tx, uintptr_t finish_cache)
  {
      foreach (OrecList, i, tx->r_orecs) {
      // read this orec
          uintptr_t ivt = (*i)->v.all;
          // if it has a timestamp of ts_cache or greater, abort
      if (ivt > tx->ts_cache)
          TxAbortWrapper(tx);
      }
      // remember that at this time, we were still valid
      tx->ts_cache = finish_cache;
  }

  /**
   *   Cohorts Tx Abort Wrapper for commit
   *   for abort inside commit. Since we already have order, we need
   *   to mark self as last_complete, increase total committed tx
   */
  void
  Cohorts::TxAbortWrapper(TxThread* tx)
  {
      // increase total number of committed tx
      ADD(&committed, 1);

      // set self as completed
      last_complete.val = tx->order;

      // abort
      tx->tmabort(tx);
  }

  /**
   *  Switch to Cohorts:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   *    Also, last_complete must equal timestamp
   *
   */
  void
  Cohorts::onSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
  }
}

namespace stm {
  /**
   *  Cohorts initialization
   */
  template<>
  void initTM<Cohorts>()
  {
      // set the name
      stms[Cohorts].name      = "Cohorts";
      // set the pointers
      stms[Cohorts].begin     = ::Cohorts::begin;
      stms[Cohorts].commit    = ::Cohorts::commit_ro;
      stms[Cohorts].read      = ::Cohorts::read_ro;
      stms[Cohorts].write     = ::Cohorts::write_ro;
      stms[Cohorts].rollback  = ::Cohorts::rollback;
      stms[Cohorts].irrevoc   = ::Cohorts::irrevoc;
      stms[Cohorts].switcher  = ::Cohorts::onSwitchTo;
      stms[Cohorts].privatization_safe = true;
  }
}

