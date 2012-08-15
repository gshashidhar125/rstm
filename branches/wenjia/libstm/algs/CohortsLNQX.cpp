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
 *  CohortsLNQX Implementation
 *
 *  CohortsLazy Norec Queue
 *  Early Seal
 *
 */
#include "algs.hpp"
#include "../Diagnostics.hpp"

namespace stm
{
  // inline or not should depends on configurations
  TM_FASTCALL bool CohortsLNQXValidate(TxThread* tx);
  TM_FASTCALL void* CohortsLNQXReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* CohortsLNQXReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void CohortsLNQXWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsLNQXWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void CohortsLNQXCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void CohortsLNQXCommitRW(TX_LONE_PARAMETER);
  TM_FASTCALL void CohortsLNQXCommitTurbo(TX_LONE_PARAMETER);
  TM_FASTCALL void* CohortsLNQXReadTurbo(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void CohortsLNQXWriteTurbo(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));

  /**
   *  CohortsLNQX begin:
   *  CohortsLNQX has a strict policy for transactions to begin. At first,
   *  every tx can start, until one of the tx is ready to commit. Then no
   *  tx is allowed to start until all the transactions finishe their
   *  commits.
   */
  void CohortsLNQXBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
    S1:
      // wait if I'm blocked
      while (q != NULL || sealed.val == 1);

      // set started
      tx->status = COHORTS_STARTED;
      WBR;

      // double check no one is ready to commit
      if (q != NULL || sealed.val == 1) {
          // [mfs] verify that no fences are needed here
          // [wer210] I don't think we need fence here...
          //          although verified by only testing benches
          tx->status = COHORTS_COMMITTED;
          goto S1;
      }
      // reset threadlocal variables
      tx->turn.val = COHORTS_NOTDONE;
      tx->cohort_writes = 0;
      tx->cohort_reads = 0;

      // test if we need to do a early seal based on abort number
      if (tx->cohort_aborts == ABORT_EARLYSEAL.val) {
          atomicswap32(&sealed.val, 1);
          tx->cohort_aborts = 0;
      }
  }

  /**
   *  CohortsLNQX commit (read-only):
   */
  void CohortsLNQXCommitRO(TX_LONE_PARAMETER)
  {
      // [mfs] Do we need a read-write fence to ensure all reads are done
      //       before we write to tx->status?
      TX_GET_TX_INTERNAL;
      // mark self status
      tx->status = COHORTS_COMMITTED;

      // clean up
      tx->vlist.reset();
      OnROCommit(tx);
  }

  /**
   *  CohortsLNQX commit_turbo (for write inplace tx use):
   */
  void CohortsLNQXCommitTurbo(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // Mark self committed
      tx->status = COHORTS_COMMITTED;

      // clean up
      tx->vlist.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, CohortsLNQXReadRO, CohortsLNQXWriteRO, CohortsLNQXCommitRO);
  }

  /**
   *  CohortsLNQX commit (writing context):
   */
  void CohortsLNQXCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // pointer to the predecessor node in the queue
      struct cohorts_node_t* pred;

      pred = __sync_lock_test_and_set(&q, &(tx->turn));

      // Mark self pending to commit
      tx->status = COHORTS_CPENDING;
      WBR;

      // Not first one? wait for your turn
      if (pred != NULL)
          while (pred->val != COHORTS_DONE);
      else {
          // First one in a cohort waits until all tx are ready to commit
          for (uint32_t i = 0; i < threadcount.val; ++i)
              while (threads[i]->status == COHORTS_STARTED);
      }
      // Everyone must validate read
      if (!CohortsLNQXValidate(tx)) {
          // count the number of aborts
          tx->cohort_aborts ++;
          // mark self done
          tx->turn.val = COHORTS_DONE;
      if (q == &(tx->turn)) {
              sealed.val = 0;
              q = NULL;
          }
          tmabort();
      }

      // Do write back
      tx->writes.writeback();
      CFENCE;

      // Mark self status
      tx->turn.val = COHORTS_DONE;

      // last one in a cohort reset q
      if (q == &(tx->turn)) {
          sealed.val = 0;
          q = NULL;
      }

      // commit all frees, reset all lists
      tx->vlist.reset();
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, CohortsLNQXReadRO, CohortsLNQXWriteRO, CohortsLNQXCommitRO);
  }

  /**
   *  CohortsLNQX read (read-only transaction)
   */
  void* CohortsLNQXReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;

      tx->cohort_reads++;
      // test if we need to do a early seal based on write number
      if (tx->cohort_reads == READ_EARLYSEAL.val)
          atomicswap32(&sealed.val, 1);

      void* tmp = *addr;
      STM_LOG_VALUE(tx, addr, tmp, mask);
      return tmp;
  }

  /**
   *  CohortsLNQX read_turbo (for write in place tx use)
   */
  void* CohortsLNQXReadTurbo(TX_FIRST_PARAMETER_ANON STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  CohortsLNQX read (writing transaction)
   */
  void* CohortsLNQXReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
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
   *  CohortsLNQX write (read-only context): for first write
   */
  void CohortsLNQXWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, CohortsLNQXReadRW, CohortsLNQXWriteRW, CohortsLNQXCommitRW);
  }
  /**
   *  CohortsLNQX write_turbo: for write in place tx
   */
  void CohortsLNQXWriteTurbo(TX_FIRST_PARAMETER_ANON STM_WRITE_SIG(addr,val,mask))
  {
      // [mfs] ultimately this should use a macro that employs the mask
      *addr = val;
  }

  /**
   *  CohortsLNQX write (writing context)
   */
  void CohortsLNQXWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;

      // record the new value in a redo log
      //
      // [mfs] we might get better instruction scheduling if we put this code
      //       first, and then did the check.
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));

      tx->cohort_writes++;
      // test if we need to do a early seal based on write number
      if (tx->cohort_writes == WRITE_EARLYSEAL.val)
          atomicswap32(&sealed.val, 1);
  }

  /**
   *  CohortsLNQX unwinder:
   */
  void CohortsLNQXRollback(STM_ROLLBACK_SIG(tx, except, len))
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
   *  CohortsLNQX in-flight irrevocability:
   */
  bool CohortsLNQXIrrevoc(TxThread*)
  {
      UNRECOVERABLE("CohortsLNQX Irrevocability not yet supported");
      return false;
  }

  /**
   *  CohortsLNQX validation for commit: check that all reads are valid
   */
  bool CohortsLNQXValidate(TxThread* tx)
  {
      foreach (ValueList, i, tx->vlist) {
          bool valid = STM_LOG_VALUE_IS_VALID(i, tx);
          if (!valid) return false;
      }
      return true;
  }

  /**
   *  Switch to CohortsLNQX:
   */
  void CohortsLNQXOnSwitchTo()
  {
      // when switching algs, mark all tx committed status
      for (uint32_t i = 0; i < threadcount.val; ++i) {
          threads[i]->status = COHORTS_COMMITTED;
      }

      //[wer210] get a configuration for CohortsLNQX
      // write
      const char* cfgwrites = "-1";
      const char* cfgstring1 = getenv("STM_WRITES");
      if (cfgstring1)
          cfgwrites = cfgstring1;

      switch (*cfgwrites) {
        case '-': WRITE_EARLYSEAL.val = -1; break;
        case '0': WRITE_EARLYSEAL.val = 0; break;
        case '1': WRITE_EARLYSEAL.val = 1; break;
        case '2': WRITE_EARLYSEAL.val = 2; break;
        case '3': WRITE_EARLYSEAL.val = 3;
      };

      // read
      const char* cfgreads = "-1";
      const char* cfgstring2 = getenv("STM_READS");
      if (cfgstring2)
          cfgreads = cfgstring2;

      switch (*cfgreads) {
        case '-': READ_EARLYSEAL.val = -1; break;
        case '0': READ_EARLYSEAL.val = 0; break;
        case '1': READ_EARLYSEAL.val = 1; break;
        case '2': READ_EARLYSEAL.val = 2; break;
        case '3': READ_EARLYSEAL.val = 3;
      };

      // abort
      const char* cfgaborts = "-1";
      const char* cfgstring3 = getenv("STM_ABORTS");
      if (cfgstring3)
          cfgaborts = cfgstring3;

      switch (*cfgaborts) {
        case '-': ABORT_EARLYSEAL.val = -1; break;
        case '0': ABORT_EARLYSEAL.val = 0; break;
        case '1': ABORT_EARLYSEAL.val = 1; break;
        case '2': ABORT_EARLYSEAL.val = 2; break;
        case '3': ABORT_EARLYSEAL.val = 3;
      };
      printf("Use STM_READS = %d, STM_WRITES = %d, STM_ABORTS = %d\n",
             READ_EARLYSEAL.val, WRITE_EARLYSEAL.val, ABORT_EARLYSEAL.val);
  }
}


DECLARE_SIMPLE_METHODS_FROM_TURBO(CohortsLNQX)
REGISTER_FGADAPT_ALG(CohortsLNQX, "CohortsLNQX", true)

#ifdef STM_ONESHOT_ALG_CohortsLNQX
DECLARE_AS_ONESHOT_TURBO(CohortsLNQX)
#endif
