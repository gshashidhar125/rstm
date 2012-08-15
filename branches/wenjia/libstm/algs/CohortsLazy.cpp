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
 *  Cohorts with only one CAS in CommitRW to get an order. Using
 *  txn local status instead of 3 global accumulators.
 *
 * "Lazy" isn't a good name for this... if I understand correctly, this is
 * Cohorts with a distributed mechanism for tracking the state of the cohort.
 */
#include "algs.hpp"
#include "../Diagnostics.hpp"

namespace stm
{
  TM_FASTCALL void* CohortsLazyReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* CohortsLazyReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void CohortsLazyWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsLazyWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsLazyCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void CohortsLazyCommitRW(TX_LONE_PARAMETER);
  TM_FASTCALL void CohortsLazyValidate(TxThread* tx);

  /**
   *  CohortsLazy begin:
   *  CohortsLazy has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishes their
   *  commits.
   */
  void CohortsLazyBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
    S1:
      // wait if I'm blocked
      while(gatekeeper.val == 1);

      // set started
      tx->status = COHORTS_STARTED;
      WBR;

      // double check no one is ready to commit
      if (gatekeeper.val == 1){
          tx->status = COHORTS_COMMITTED;
          goto S1;
      }

      //begin
      tx->allocator.onTxBegin();

      // get time of last finished txn
      tx->ts_cache = last_complete.val;
  }

  /**
   *  CohortsLazy commit (read-only):
   */
  void
  CohortsLazyCommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // mark self status
      tx->status = COHORTS_COMMITTED;

      // clean up
      tx->r_orecs.reset();
      OnROCommit(tx);
  }

  /**
   *  CohortsLazy commit (writing context):
   *
   */
  void
  CohortsLazyCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // Mark a global flag, no one is allowed to begin now
      //
      // [mfs] If we used ADD on gatekeper, we wouldn't need to do a FAI on
      //       timestamp.val later
      gatekeeper.val = 1;

      // Mark self pending to commit
      tx->status = COHORTS_CPENDING;

      // Get an order
      tx->order = 1 + faiptr(&timestamp.val);

      // For later use, indicates if I'm the last tx in this cohort
      bool lastone = true;

      // Wait until all tx are ready to commit
      //
      // [mfs] Some key information is lost here.  If I am the first
      // transaction, then when I do this loop, I could easily figure out
      // exactly how many transactions are in the cohort.  If I then set that
      // value in a global, nobody else would later have to go searching around
      // to try to figure out if they are the oldest or not.
      for (uint32_t i = 0; i < threadcount.val; ++i)
          while (threads[i]->status == COHORTS_STARTED);

      // Wait for my turn
      while (last_complete.val != (uintptr_t)(tx->order - 1));

      // If I'm the first one in this cohort, no validation, else validate
      if (tx->order != last_order.val)
          CohortsLazyValidate(tx);

      // mark orec, do write back
      foreach (WriteSet, i, tx->writes) {
          orec_t* o = get_orec(i->addr);
          o->v.all = tx->order;
          *i->addr = i->val;
      }
      CFENCE;
      // Mark self as done
      last_complete.val = tx->order;

      // Mark self status
      tx->status = COHORTS_COMMITTED;
      WBR;

      // Am I the last one?
      for (uint32_t i = 0; lastone != false && i < threadcount.val; ++i)
          lastone &= (threads[i]->status != COHORTS_CPENDING);

      // If I'm the last one, release gatekeeper lock
      if (lastone) {
          last_order.val = tx->order + 1;
          gatekeeper.val = 0;
      }

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, CohortsLazyReadRO, CohortsLazyWriteRO, CohortsLazyCommitRO);
  }

  /**
   *  CohortsLazy read (read-only transaction)
   */
  void*
  CohortsLazyReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // log orec
      tx->r_orecs.insert(get_orec(addr));
      return *addr;
  }

  /**
   *  CohortsLazy read (writing transaction)
   */
  void*
  CohortsLazyReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
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
  CohortsLazyWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, CohortsLazyReadRW, CohortsLazyWriteRW, CohortsLazyCommitRW);
  }

  /**
   *  CohortsLazy write (writing context)
   */
  void
  CohortsLazyWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CohortsLazy unwinder:
   */
  void
  CohortsLazyRollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();

      PostRollback(tx);
  }

  /**
   *  CohortsLazy in-flight irrevocability:
   */
  bool
  CohortsLazyIrrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsLazy Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsLazy validation for commit: check that all reads are valid
   */
  void
  CohortsLazyValidate(TxThread* tx)
  {
      // [mfs] use the luke trick on this loop
      foreach (OrecList, i, tx->r_orecs) {
          // read this orec
          uintptr_t ivt = (*i)->v.all;
          // If orec changed, abort
          if (ivt > tx->ts_cache) {
              // Mark self as done
              last_complete.val = tx->order;
              // Mark self status
              tx->status = COHORTS_COMMITTED;
              WBR;

              // Am I the last one?
              bool l = true;
              for (uint32_t i = 0; l != false && i < threadcount.val; ++i)
                  l &= (threads[i]->status != COHORTS_CPENDING);

              // If I'm the last one, release gatekeeper.val lock
              if (l) {
                  last_order.val = tx->order + 1;
                  gatekeeper.val = 0;
              }
              tmabort();
          }
      }
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
  CohortsLazyOnSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
      // when switching algs, mark all tx committed status
      for (uint32_t i = 0; i < threadcount.val; ++i) {
          threads[i]->status = COHORTS_COMMITTED;
      }
  }
}


DECLARE_SIMPLE_METHODS_FROM_NORMAL(CohortsLazy)
REGISTER_FGADAPT_ALG(CohortsLazy, "CohortsLazy", true)

#ifdef STM_ONESHOT_ALG_CohortsLazy
DECLARE_AS_ONESHOT_NORMAL(CohortsLazy)
#endif
