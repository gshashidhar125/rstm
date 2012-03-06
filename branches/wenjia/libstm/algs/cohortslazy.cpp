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
 *  CohortsLazy Implementation
 *
 *  CohortsLazy has 4 stages. 1) Nobody is running. If anyone starts,
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
#define ADD __sync_add_and_fetch
#define SUB __sync_sub_and_fetch

#define COHORTS_COMMITTED 0
#define COHORTS_STARTED   1
#define COHORTS_CPENDING  2

using stm::TxThread;
using stm::threads;
using stm::threadcount;
using stm::last_complete;
using stm::timestamp;
using stm::timestamp_max;
using stm::WriteSet;
using stm::OrecList;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::orec_t;
using stm::get_orec;
using stm::gatekeeper;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  struct CohortsLazy {
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
      static NOINLINE bool validate(TxThread* tx);
  };

  /**
   *  CohortsLazy begin:
   *  CohortsLazy has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  bool
  CohortsLazy::begin(TxThread* tx)
  {
    S1:
      // wait if I'm blocked
      while(gatekeeper == 1);

      // set started flag
      tx->status = COHORTS_STARTED;
      WBR;

      // double check no one is ready to commit
      if (gatekeeper == 1){
          tx->status = COHORTS_COMMITTED;
          goto S1;
      }

      //begin
      tx->allocator.onTxBegin();

      // get time of last finished txn
      tx->ts_cache = last_complete.val;

      return true;
  }

  /**
   *  CohortsLazy commit (read-only):
   */
  void
  CohortsLazy::commit_ro(TxThread* tx)
  {
      // mark end
      tx->status = COHORTS_COMMITTED;

      // clean up
      tx->r_orecs.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  CohortsLazy commit (writing context):
   *
   */
  void
  CohortsLazy::commit_rw(TxThread* tx)
  {
      // mark a global flag
      gatekeeper = 1;

      // mark self pending to commit
      tx->status = COHORTS_CPENDING;

      // increment num of tx ready to commit, and use it as the order
      tx->order = 1 + faiptr(&timestamp.val);

      // wait until all tx are in commit
      for (uint32_t i = 0; i < threadcount.val; ++i)
          while (threads[i]->status == COHORTS_STARTED);

      // Wait for my turn
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      //[TODO] if I'm the first one, no validation, else validate
      bool val = validate(tx);

      if (val)
          // mark orec
          foreach (WriteSet, i, tx->writes) {
              // get orec
              orec_t* o = get_orec(i->addr);
              // mark orec
              o->v.all = tx->order;
              CFENCE;
              *i->addr = i->val;
          }

      // mark self as done
      last_complete.val = tx->order;
      CFENCE;

      // mark self status
      tx->status = COHORTS_COMMITTED;
      WBR;

      // Am I the last one?
      bool lastone = true;
      for (uint32_t i = 0; i < threadcount.val; ++i)
          if (threads[i]->status == COHORTS_CPENDING)
              lastone = false;

      // If I'm the last one, release blocked flag
      if (lastone) {
          gatekeeper = 0;
          WBR;
      }
      // If validation failed
      if (!val) {
          tx->tmabort(tx);
          return;
      }
      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CohortsLazy read (read-only transaction)
   */
  void*
  CohortsLazy::read_ro(STM_READ_SIG(tx,addr,))
  {
      // log orec
      tx->r_orecs.insert(get_orec(addr));
      return *addr;
  }

  /**
   *  CohortsLazy read (writing transaction)
   */
  void*
  CohortsLazy::read_rw(STM_READ_SIG(tx,addr,mask))
  {
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // log orec
      tx->r_orecs.insert(get_orec(addr));

      void* tmp = *addr;
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      return tmp;
  }

  /**
   *  CohortsLazy write (read-only context): for first write
   */
  void
  CohortsLazy::write_ro(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  CohortsLazy write (writing context)
   */
  void
  CohortsLazy::write_rw(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CohortsLazy unwinder:
   */
  stm::scope_t*
  CohortsLazy::rollback(STM_ROLLBACK_SIG(tx, except, len))
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
   *  CohortsLazy in-flight irrevocability:
   */
  bool
  CohortsLazy::irrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsLazy Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsLazy validation for commit: check that all reads are valid
   */
  bool
  CohortsLazy::validate(TxThread* tx)
  {
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // If orec changed, abort
          if (ivt > tx->ts_cache)
              return false;
      }
      return true;
  }

  /**
   *  Switch to CohortsLazy:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   */
  void
  CohortsLazy::onSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
      // when switching algs, mark all tx unblocking and not started
      for (uint32_t i = 0; i < threadcount.val; ++i) {
          threads[i]->status = COHORTS_COMMITTED;
      }
  }
}

namespace stm {
  /**
   *  CohortsLazy initialization
   */
  template<>
  void initTM<CohortsLazy>()
  {
      // set the name
      stms[CohortsLazy].name      = "CohortsLazy";
      // set the pointers
      stms[CohortsLazy].begin     = ::CohortsLazy::begin;
      stms[CohortsLazy].commit    = ::CohortsLazy::commit_ro;
      stms[CohortsLazy].read      = ::CohortsLazy::read_ro;
      stms[CohortsLazy].write     = ::CohortsLazy::write_ro;
      stms[CohortsLazy].rollback  = ::CohortsLazy::rollback;
      stms[CohortsLazy].irrevoc   = ::CohortsLazy::irrevoc;
      stms[CohortsLazy].switcher  = ::CohortsLazy::onSwitchTo;
      stms[CohortsLazy].privatization_safe = true;
  }
}

