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
 *  CohortsEN Implementation
 *
 *  CohortsEN is CohortsNorec with inplace write if I'm the last one in the
 *  cohort.
 */

#include "../profiling.hpp"
#include "../algs.hpp"
#include "../RedoRAWUtils.hpp"

using stm::TxThread;
using stm::last_complete;
using stm::timestamp;
using stm::timestamp_max;
using stm::WriteSet;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;

using stm::ValueList;
using stm::ValueListEntry;
using stm::started;
using stm::cpending;
using stm::committed;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  volatile uintptr_t inplace = 0;
  NOINLINE bool validate(TxThread* tx);

  struct CohortsEN {
      static void begin(TX_LONE_PARAMETER);
      static TM_FASTCALL void* read_ro(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_rw(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_turbo(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_turbo(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void commit_ro(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_rw(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_turbo(TX_LONE_PARAMETER);

      static void rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
  };

  /**
   *  CohortsEN begin:
   *  CohortsEN has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  void CohortsEN::begin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();

    S1:
      // wait until everyone is committed
      while (cpending.val != committed.val);

      // before tx begins, increase total number of tx
      faiptr(&started.val);

      // [NB] we must double check no one is ready to commit yet
      // and no one entered in place write phase(turbo mode)
      if (cpending.val > committed.val || inplace == 1){
          faaptr(&started.val, -1);
          goto S1;
      }
  }

  /**
   *  CohortsEN commit (read-only):
   */
  void
  CohortsEN::commit_ro(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // decrease total number of tx started
      faaptr(&started.val, -1);

      // clean up
      tx->vlist.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  CohortsEN commit (in place write commit): no validation, no write back
   *  no other thread touches cpending
   */
  void
  CohortsEN::commit_turbo(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // increase # of tx waiting to commit, and use it as the order
      tx->order = 1 + faiptr(&cpending.val);

      // clean up
      tx->vlist.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);

      // wait for my turn
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      // reset in place write flag
      inplace = 0;

      // increase # of committed
      committed.val ++;
      CFENCE;

      // mark self as done
      last_complete.val = tx->order;
  }

  /**
   *  CohortsEN commit (writing context):
   *
   *  RW commit is operated in turns. Transactions will be allowed to commit
   *  in an order which is given at the beginning of commit.
   */
  void
  CohortsEN::commit_rw(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // order of first tx in cohort
      int32_t first = last_complete.val + 1;
      CFENCE;

      // increase # of tx waiting to commit, and use it as the order
      tx->order = 1 + faiptr(&cpending.val);

      // Wait for my turn
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      // Wait until all tx are ready to commit
      while (cpending.val < started.val);

      // If in place write occurred, all tx validate reads
      // Otherwise, only first one skips validation
      if (inplace == 1 || tx->order != first)
          if (!validate(tx)) {
              committed.val++;
              CFENCE;
              last_complete.val = tx->order;
              tx->tmabort();
          }

      // do write back
      tx->writes.writeback();

      // increase total number of committed tx
      committed.val++;
      CFENCE;

      // mark self as done
      last_complete.val = tx->order;

      // commit all frees, reset all lists
      tx->vlist.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CohortsEN read_turbo
   */
  void*
  CohortsEN::read_turbo(TX_FIRST_PARAMETER_ANON STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  CohortsEN read (read-only transaction)
   */
  void*
  CohortsEN::read_ro(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      void *tmp = *addr;
      STM_LOG_VALUE(tx, addr, tmp, mask);
      return tmp;
  }

  /**
   *  CohortsEN read (writing transaction)
   */
  void*
  CohortsEN::read_rw(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      void* tmp = *addr;
      STM_LOG_VALUE(tx, addr, tmp, mask);
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      return tmp;
  }

  /**
   *  CohortsEN write (read-only context): for first write
   */
  void
  CohortsEN::write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {TX_GET_TX_INTERNAL;

#ifndef TRY
      // If everyone else is ready to commit, do in place write
      if (cpending.val + 1 == started.val) {
          // set up flag indicating in place write starts
          atomicswapptr(&inplace, 1);
          // double check is necessary
          if (cpending.val + 1 == started.val) {
              // in place write
              *addr = val;
              // go turbo mode
              stm::OnFirstWrite(read_turbo, write_turbo, commit_turbo);
              return;
          }
          // reset flag
          inplace = 0;
      }
#endif
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      stm::OnFirstWrite(read_rw, write_rw, commit_rw);
  }

  /**
   *  CohortsEN write (in place write)
   */
  void
  CohortsEN::write_turbo(TX_FIRST_PARAMETER_ANON STM_WRITE_SIG(addr,val,mask))
  {
      *addr = val; // in place write
  }

  /**
   *  CohortsEN write (writing context)
   */
  void
  CohortsEN::write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
#ifdef TRY
      // Try to go turbo when "writes.size(TX_LONE_PARAMETER) >= TIMES"
      if (tx->writes.size(TX_LONE_PARAMETER) >= TIMES && cpending.val + 1 == started.val) {
          // set up flag indicating in place write starts
          atomicswapptr(&inplace, 1);
          // double check is necessary
          if (cpending.val + 1 == started.val) {
              // write back
              tx->writes.writeback(TX_LONE_PARAMETER);
              // in place write
              *addr = val;
              // go turbo mode
              OnFirstWrite(tx, read_turbo, write_turbo, commit_turbo);
              return;
          }
          // reset flag
          inplace = 0;
      }
#endif
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CohortsEN unwinder:
   */
  void
  CohortsEN::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists
      tx->vlist.reset();
      tx->writes.reset();

      PostRollback(tx);
  }

  /**
   *  CohortsEN in-flight irrevocability:
   */
  bool
  CohortsEN::irrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsEN Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsEN validation for commit: check that all reads are valid
   */
  bool
  validate(TxThread* tx)
  {
      foreach (ValueList, i, tx->vlist) {
          bool valid = STM_LOG_VALUE_IS_VALID(i, tx);
          if (!valid) return false;
      }
      return true;
  }

  /**
   *  Switch to CohortsEN:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   */
  void
  CohortsEN::onSwitchTo()
  {
      last_complete.val = 0;
      inplace = 0;
  }
}

namespace stm {
  /**
   *  CohortsEN initialization
   */
  template<>
  void initTM<CohortsEN>()
  {
      // set the name
      stms[CohortsEN].name      = "CohortsEN";
      // set the pointers
      stms[CohortsEN].begin     = ::CohortsEN::begin;
      stms[CohortsEN].commit    = ::CohortsEN::commit_ro;
      stms[CohortsEN].read      = ::CohortsEN::read_ro;
      stms[CohortsEN].write     = ::CohortsEN::write_ro;
      stms[CohortsEN].rollback  = ::CohortsEN::rollback;
      stms[CohortsEN].irrevoc   = ::CohortsEN::irrevoc;
      stms[CohortsEN].switcher  = ::CohortsEN::onSwitchTo;
      stms[CohortsEN].privatization_safe = true;
  }
}

