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
 *  OrecELA_AOU Implementation
 *
 *    This is similar to the Detlefs algorithm for privatization-safe STM,
 *    TL2-IP, and [Marathe et al. ICPP 2008].  We use commit time ordering to
 *    ensure that there are no delayed cleanup problems, we poll the timestamp
 *    variable to address doomed transactions, but unlike the above works, we
 *    use TinySTM-style extendable timestamps instead of TL2-style timestamps,
 *    which sacrifices some publication safety.
 */

#include "algs.hpp"

// this variant has serialization for privatization, with AOU for polling

namespace stm
{
  TM_FASTCALL void* OrecELA_AOUReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* OrecELA_AOUReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void OrecELA_AOUWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void OrecELA_AOUWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void OrecELA_AOUCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void OrecELA_AOUCommitRW(TX_LONE_PARAMETER);

  // [mfs] If I understand the AOU spec implementation correctly, this is
  //       what we use as the handler on an AOU alert
  NOINLINE void OrecELA_AOU_Handler(void* arg, Watch_Descriptor* w)
  {
      // [mfs] This isn't sufficient if we aren't using the default TLS
      //       access mechanism:
      TX_GET_TX_INTERNAL;

      // If we just took an AOU alert, and are in this code, then we need to
      // decide whether we can keep running.  This basically just means we
      // need to validate...
      uintptr_t ts = timestamp.val;
      w->locs[0].val = (uint64_t)ts;    // Update the expected value

      // optimized validation since we don't hold any locks
      foreach (OrecList, i, tx->r_orecs) {
          // if orec locked or newer than start time, abort
          if ((*i)->v.all > tx->start_time) {
              // NB: we aren't in an AOU context, so it is safe to abort here
              // without dropping AOU lines.  However, we need to reset our
              // AOU context
              AOU_reset(tx->aou_context);
              tmabort();
          }
      }

      // careful here: we can't scale the start time past last_complete.val,
      // unless we want to re-introduce the need for prevalidation on every
      // read.
      uintptr_t cs = last_complete.val;
      tx->start_time = (ts < cs) ? ts : cs;
  }

  /**
   *  OrecELA_AOU begin:
   *
   *    We need a starting point for the transaction.  If an in-flight
   *    transaction is committed, but still doing writeback, we can either start
   *    at the point where that transaction had not yet committed, or else we can
   *    wait for it to finish writeback.  In this code, we choose the former
   *    option.
   */
  void OrecELA_AOUBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
      // Start after the last cleanup, instead of after the last commit, to
      // avoid spinning in Begin(TX_LONE_PARAMETER)
      tx->start_time = last_complete.val;
      tx->end_time = 0;

      // set up AOU context for every thread if it doesn't have one already...
      //
      // [mfs] This is not the optimal placement for this code, but will do
      //       for now
      if (__builtin_expect(!tx->aou_context, false)) {
          tx->aou_context = AOU_init(OrecELA_AOU_Handler, NULL, /*max_locs = */ 1);
          if (tx->aou_context == NULL)
              printf("Uh-Oh, context is null\n");
      }

      // turn on AOU tracking support
      AOU_start(tx->aou_context);

      // track the timestamp... note that we ignore the return value
      AOU_load(tx->aou_context, (uint64_t*)&timestamp.val);
  }

  /**
   *  OrecELA_AOU commit (read-only):
   *
   *    RO commit is trivial
   */
  void
  OrecELA_AOUCommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;

      // stop AOU tracking...
      AOU_stop(tx->aou_context);

      // standard RO commit stuff...
      tx->r_orecs.reset();
      OnROCommit(tx);
  }

  /**
   *  OrecELA_AOU commit (writing context):
   *
   *    OrecELA_AOU commit is like LLT: we get the locks, increment the counter, and
   *    then validate and do writeback.  As in other systems, some increments
   *    lead to skipping validation.
   *
   *    After writeback, we use a second, trailing counter to know when all txns
   *    who incremented the counter before this tx are done with writeback.  Only
   *    then can this txn mark its writeback complete.
   */
  void
  OrecELA_AOUCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;

      // stop AOU tracking...
      AOU_stop(tx->aou_context);
      AOU_reset(tx->aou_context);

      // acquire locks
      foreach (WriteSet, i, tx->writes) {
          // get orec, read its version#
          orec_t* o = get_orec(i->addr);
          uintptr_t ivt = o->v.all;

          // if orec not locked, lock it and save old to orec.p
          if (ivt <= tx->start_time) {
              // abort if cannot acquire
              if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                  tmabort();
              // save old version to o->p, log lock
              o->p = ivt;
              tx->locks.insert(o);
          }
          // else if we don't hold the lock abort
          else if (ivt != tx->my_lock.all) {
              tmabort();
          }
      }

      // increment the global timestamp if we have writes
      tx->end_time = 1 + faiptr(&timestamp.val);

      // skip validation if possible
      if (tx->end_time != (tx->start_time + 1)) {
          foreach (OrecList, i, tx->r_orecs) {
              // read this orec
              uintptr_t ivt = (*i)->v.all;
              if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
                  tmabort();
          }
      }

      // run the redo log
      tx->writes.writeback();
      CFENCE;
      // release locks
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = tx->end_time;
      CFENCE;

      // now ensure that transactions depart from stm_end in the order that
      // they incremend the timestamp.  This avoids the "deferred update"
      // half of the privatization problem.
      while (last_complete.val != (tx->end_time - 1))
          spin64();
      last_complete.val = tx->end_time;

      // clean-up
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      OnRWCommit(tx);
      ResetToRO(tx, OrecELA_AOUReadRO, OrecELA_AOUWriteRO, OrecELA_AOUCommitRO);
  }

  /**
   *  OrecELA_AOU read (read-only transaction)
   *
   *    This is a traditional orec read for systems with extendable timestamps.
   *    However, we also poll the timestamp counter and validate any time a new
   *    transaction has committed, in order to catch doomed transactions.
   */
  void*
  OrecELA_AOUReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      while (true) {
          // read the location
          void* tmp = *addr;
          CFENCE;
          // check the orec.  Note: we don't need prevalidation because we
          // have a global clean state via the last_complete.val field.
          id_version_t ivt;
          ivt.all = o->v.all;

          // common case: new read to uncontended location
          if (ivt.all <= tx->start_time) {
              tx->r_orecs.insert(o);
              // [mfs] Note that we don't have a privtest call, since we are using AOU
              return tmp;
          }

          // if lock held, spin and retry
          if (ivt.fields.lock) {
              spin64();
              continue;
          }

          // unlocked but too new... validate and scale forward
          uintptr_t newts = timestamp.val;
          foreach (OrecList, i, tx->r_orecs) {
              // if orec locked or newer than start time, abort
              if ((*i)->v.all > tx->start_time) {
                  // stop AOU tracking...
                  AOU_stop(tx->aou_context);
                  AOU_reset(tx->aou_context);
                  // now we can abort, knowing that we're in a safe state in
                  // the abort handler
                  tmabort();
              }
          }

          uintptr_t cs = last_complete.val;
          // need to pick cs or newts
          tx->start_time = (newts < cs) ? newts : cs;
      }
  }

  /**
   *  OrecELA_AOU read (writing transaction)
   *
   *    Identical to RO case, but with write-set lookup first
   */
  void*
  OrecELA_AOUReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // reuse the ReadRO barrier, which is adequate here---reduces LOC
      void* val = OrecELA_AOUReadRO(TX_FIRST_ARG addr STM_MASK(mask));
      REDO_RAW_CLEANUP(val, found, log, mask);
      return val;
  }

  /**
   *  OrecELA_AOU write (read-only context)
   *
   *    Simply buffer the write and switch to a writing context
   */
  void
  OrecELA_AOUWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, OrecELA_AOUReadRW, OrecELA_AOUWriteRW, OrecELA_AOUCommitRW);
  }

  /**
   *  OrecELA_AOU write (writing context)
   *
   *    Simply buffer the write
   */
  void
  OrecELA_AOUWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  OrecELA_AOU unwinder:
   *
   *    This is a standard orec unwind function.  The only catch is that if a
   *    transaction aborted after incrementing the timestamp, it must wait its
   *    turn and then increment the trailing timestamp, to keep the two counters
   *    consistent.
   */
  void
  OrecELA_AOURollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // release locks and restore version numbers
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = (*i)->p;
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();

      CFENCE;
      // if we aborted after incrementing the timestamp, then we have to
      // participate in the global cleanup order to support our solution to
      // the deferred update half of the privatization problem.
      //
      // NB:  Note that end_time is always zero for restarts and retrys
      if (tx->end_time != 0) {
          while (last_complete.val < (tx->end_time - 1))
              spin64();
          last_complete.val = tx->end_time;
      }
      PostRollback(tx);
      ResetToRO(tx, OrecELA_AOUReadRO, OrecELA_AOUWriteRO, OrecELA_AOUCommitRO);
  }

  /**
   *  OrecELA_AOU in-flight irrevocability: use abort-and-restart
   */
  bool OrecELA_AOUIrrevoc(TxThread*)
  {
      return false;
  }

  /**
   *  OrecELA_AOU validation
   *
   *    an in-flight transaction must make sure it isn't suffering from the
   *    "doomed transaction" half of the privatization problem.  We can get that
   *    effect by calling this after every transactional read (actually every
   *    read that detects that some new transaction has committed).
   *
   *  NB: this is dead code.
   */
  void
  OrecELA_AOUPrivtest(TxThread* tx, uintptr_t ts)
  {
      // optimized validation since we don't hold any locks
      foreach (OrecList, i, tx->r_orecs) {
          // if orec locked or newer than start time, abort
          if ((*i)->v.all > tx->start_time) {
              // NB: we aren't in an AOU context, so it is safe to abort here
              // without dropping AOU lines.  However, we need to reset our
              // AOU context
              AOU_reset(tx->aou_context);
              tmabort();
          }
      }
      // careful here: we can't scale the start time past last_complete.val,
      // unless we want to re-introduce the need for prevalidation on every
      // read.
      uintptr_t cs = last_complete.val;
      tx->start_time = (ts < cs) ? ts : cs;
  }

  /**
   *  Switch to OrecELA_AOU:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   *    Also, last_complete must equal timestamp
   */
  void
  OrecELA_AOUOnSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
  }
}

DECLARE_SIMPLE_METHODS_FROM_NORMAL(OrecELA_AOU)
REGISTER_FGADAPT_ALG(OrecELA_AOU, "OrecELA_AOU", true)

#ifdef STM_ONESHOT_ALG_OrecELA_AOU
DECLARE_AS_ONESHOT(OrecELA_AOU)
#endif
