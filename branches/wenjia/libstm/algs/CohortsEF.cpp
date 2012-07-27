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
 *  CohortsEF Implementation
 *  CohortsEager with Filter
 *
 *  See notes in Eager and Filter implementations
 */

#include "../profiling.hpp"
#include "../algs.hpp"
#include "../RedoRAWUtils.hpp"

// define atomic operations
#define CAS __sync_val_compare_and_swap
#define ADD __sync_add_and_fetch
#define SUB __sync_sub_and_fetch

using stm::TxThread;
using stm::last_complete;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::global_filter;
using stm::started;
using stm::cpending;
using stm::committed;
using stm::last_order;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  // inplace write flag
  volatile uintptr_t inplace = 0;
  NOINLINE bool validate(TxThread* tx);

  struct CohortsEF {
      static void begin();
      static TM_FASTCALL void* read_ro(STM_READ_SIG(,));
      static TM_FASTCALL void* read_rw(STM_READ_SIG(,));
      static TM_FASTCALL void* read_turbo(STM_READ_SIG(,));
      static TM_FASTCALL void write_ro(STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_rw(STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_turbo(STM_WRITE_SIG(,,));
      static TM_FASTCALL void commit_ro();
      static TM_FASTCALL void commit_rw();
      static TM_FASTCALL void commit_turbo();

      static void rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
  };

  /**
   *  CohortsEF begin:
   *  CohortsEF has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  void CohortsEF::begin()
  {
      TxThread* tx = stm::Self;
    S1:
      // wait until everyone is committed
      while (cpending.val != committed.val);

      // before tx begins, increase total number of tx
      ADD(&started.val, 1);

      // [NB] we must double check no one is ready to commit yet
      // and no one entered in place write phase(turbo mode)
      if (cpending.val > committed.val || inplace == 1){
          SUB(&started.val, 1);
          goto S1;
      }

      tx->allocator.onTxBegin();
  }

  /**
   *  CohortsEF commit (read-only):
   */
  void
  CohortsEF::commit_ro()
  {
      TxThread* tx = stm::Self;
      // decrease total number of tx started
      SUB(&started.val, 1);

      // clean up
      tx->rf->clear();
      OnReadOnlyCommit(tx);
  }

  /**
   *  CohortsEF commit (in place write commit): no validation, no write back
   *  no other thread touches cpending.
   */
  void
  CohortsEF::commit_turbo()
  {
      TxThread* tx = stm::Self;
      // increase # of tx waiting to commit, and use it as the order
      tx->order = ADD(&cpending.val ,1);

      // clean up
      tx->rf->clear();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);

      // wait for my turn, in this case, cpending is my order
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      // I must be the last in the cohort, so clean global_filter
      global_filter->clear();

      // reset in place write flag
      inplace = 0;

      // increase # of committed
      committed.val ++;
      CFENCE;

      // mark self as done
      last_complete.val = tx->order;
  }

  /**
   *  CohortsEF commit (writing context):
   *
   *  RW commit is operated in turns. Transactions will be allowed to commit
   *  in an order which is given at the beginning of commit.
   */
  void
  CohortsEF::commit_rw()
  {
      TxThread* tx = stm::Self;
      // increase # of tx waiting to commit, and use it as the order
      tx->order = ADD(&cpending.val ,1);

      // Wait for my turn
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      // Wait until all tx are ready to commit
      while (cpending.val < started.val);

      // If in place write occurred, all tx validate reads
      // Otherwise, only first one skips validation
      if (inplace == 1 || tx->order != last_order)
          if (!validate(tx)) {
              committed.val ++;
              CFENCE;
              last_complete.val = tx->order;
              tx->tmabort();
          }

      // do write back
      tx->writes.writeback();
      WBR;

      //union tx local write filter with the global filter
      global_filter->unionwith (*(tx->wf));
      WBR;

      // If the last one in the cohort, save the order and clear the filter
      if ((uint32_t)tx->order == started.val) {
          last_order = started.val + 1;
          global_filter->clear();
      }

      // increase total number of committed tx
      committed.val ++;
      CFENCE;

      // mark self as done
      last_complete.val = tx->order;

      // commit all frees, reset all lists
      tx->rf->clear();
      tx->wf->clear();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CohortsEF read_turbo
   */
  void*
  CohortsEF::read_turbo(STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  CohortsEF read (read-only transaction)
   */
  void*
  CohortsEF::read_ro(STM_READ_SIG(addr,))
  {
      TxThread* tx = stm::Self;
      tx->rf->add(addr);
      return *addr;
  }

  /**
   *  CohortsEF read (writing transaction)
   */
  void*
  CohortsEF::read_rw(STM_READ_SIG(addr,mask))
  {
      TxThread* tx = stm::Self;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      tx->rf->add(addr);

      void* val = *addr;
      REDO_RAW_CLEANUP(val, found, log, mask);
      return val;
  }

  /**
   *  CohortsEF write (read-only context): for first write
   */
  void
  CohortsEF::write_ro(STM_WRITE_SIG(addr,val,mask))
  {
      TxThread* tx = stm::Self;
      // If everyone else is ready to commit, do in place write
      if (cpending.val + 1 == started.val) {
          // set up flag indicating in place write starts
          atomicswapptr(&inplace, 1);
          // double check is necessary
          if (cpending.val + 1 == started.val) {
              // in place write
              *addr = val;
              // add entry to the global filter
              global_filter->add(addr);
              // go turbo mode
              OnFirstWrite(tx, read_turbo, write_turbo, commit_turbo);
              return;
          }
          // reset flag
          inplace = 0;
      }
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      tx->wf->add(addr);
      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  CohortsEF write (in place write)
   */
  void
  CohortsEF::write_turbo(STM_WRITE_SIG(addr,val,mask))
  {
      *addr = val; // in place write
      // add entry to the global filter
      global_filter->add(addr);
  }

  /**
   *  CohortsEF write (writing context)
   */
  void
  CohortsEF::write_rw(STM_WRITE_SIG(addr,val,mask))
  {
      TxThread* tx = stm::Self;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      tx->wf->add(addr);
  }

  /**
   *  CohortsEF unwinder:
   */
  void
  CohortsEF::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists
      tx->rf->clear();
      if (tx->writes.size()) {
          tx->wf->clear();
          tx->writes.reset();
      }
      PostRollback(tx);
  }

  /**
   *  CohortsEF in-flight irrevocability:
   */
  bool
  CohortsEF::irrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsEF Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsEF validation for commit: check that all reads are valid
   */
  bool
  validate(TxThread* tx)
  {
      // If there is a same element in both global_filter and read_filter
      if (global_filter->intersect(tx->rf)) {
          // I'm the last one in the cohort, save the order and clear the filter
          if ((uint32_t)tx->order == started.val) {
              last_order = started.val + 1;
              global_filter->clear();
          }
          return false;
      }
      return true;
  }

  /**
   *  Switch to CohortsEF:
   *
   */
  void
  CohortsEF::onSwitchTo()
  {
      last_complete.val = 0;
      global_filter->clear();
  }
}

namespace stm {
  /**
   *  CohortsEF initialization
   */
  template<>
  void initTM<CohortsEF>()
  {
      // set the name
      stms[CohortsEF].name      = "CohortsEF";
      // set the pointers
      stms[CohortsEF].begin     = ::CohortsEF::begin;
      stms[CohortsEF].commit    = ::CohortsEF::commit_ro;
      stms[CohortsEF].read      = ::CohortsEF::read_ro;
      stms[CohortsEF].write     = ::CohortsEF::write_ro;
      stms[CohortsEF].rollback  = ::CohortsEF::rollback;
      stms[CohortsEF].irrevoc   = ::CohortsEF::irrevoc;
      stms[CohortsEF].switcher  = ::CohortsEF::onSwitchTo;
      stms[CohortsEF].privatization_safe = true;
  }
}

