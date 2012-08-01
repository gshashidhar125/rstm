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
 *  CTokenTurbo Implementation
 *
 *    This code is like CToken, except we aggressively check if a thread is the
 *    'oldest', and if it is, we switch to an irrevocable 'turbo' mode with
 *    in-place writes and no validation.
 */

#include "../profiling.hpp"
#include "../algs.hpp"
#include "../RedoRAWUtils.hpp"
#include "../UndoLog.hpp" // STM_DO_MASKED_WRITE

using stm::TxThread;
using stm::timestamp;
using stm::timestamp_max;
using stm::last_complete;
using stm::OrecList;
using stm::WriteSet;
using stm::UNRECOVERABLE;
using stm::orec_t;
using stm::get_orec;
using stm::WriteSetEntry;


/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  struct CTokenTurbo {
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
      static NOINLINE void validate(TxThread*, uintptr_t finish_cache);
  };

  /**
   *  CTokenTurbo begin:
   */
  void CTokenTurbo::begin()
  {
      TxThread* tx = stm::Self;
      tx->allocator.onTxBegin();

      // get time of last finished txn
      tx->ts_cache = last_complete.val;

      // switch to turbo mode?
      //
      // NB: this only applies to transactions that aborted after doing a write
      if (tx->ts_cache == ((uintptr_t)tx->order - 1))
          stm::GoTurbo(read_turbo, write_turbo, commit_turbo);
  }

  /**
   *  CTokenTurbo commit (read-only):
   */
  void
  CTokenTurbo::commit_ro()
  {
      TxThread* tx = stm::Self;
      tx->r_orecs.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  CTokenTurbo commit (writing context):
   *
   *  Only valid with pointer-based adaptivity
   */
  void
  CTokenTurbo::commit_rw()
  {
      TxThread* tx = stm::Self;
      // we need to transition to fast here, but not till our turn
      while (last_complete.val != ((uintptr_t)tx->order - 1))
          spin64();

      // the oldest one skip the validation
      if (tx->ts_cache != ((uintptr_t)tx->order - 1))
          // validate
          foreach (OrecList, i, tx->r_orecs) {
              // read this orec
              uintptr_t ivt = (*i)->v.all;
              // if it has a timestamp of ts_cache or greater, abort
              if (ivt > tx->ts_cache)
                  tx->tmabort();
          }

      // writeback
      if (tx->writes.size() != 0) {
          // mark every location in the write set, and perform write-back
          foreach (WriteSet, i, tx->writes) {
              orec_t* o = get_orec(i->addr);
              o->v.all = tx->order;
              CFENCE; // WBW
              *i->addr = i->val;
          }
      }

      CFENCE; // wbw between writeback and last_complete.val update
      last_complete.val = tx->order;

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CTokenTurbo commit (turbo mode):
   */
  void
  CTokenTurbo::commit_turbo()
  {
      TxThread* tx = stm::Self;
      CFENCE; // wbw between writeback and last_complete.val update
      last_complete.val = tx->order;

      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  CTokenTurbo read (read-only transaction)
   */
  void*
  CTokenTurbo::read_ro(STM_READ_SIG(addr,))
  {
      TxThread* tx = stm::Self;
      void* tmp = *addr;
      CFENCE; // RBR between dereference and orec check

      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      uintptr_t ivt = o->v.all;
      // abort if this changed since the last time I saw someone finish
      if (ivt > tx->ts_cache)
          tx->tmabort();

      // log orec
      tx->r_orecs.insert(o);

      return tmp;
  }

  /**
   *  CTokenTurbo read (writing transaction)
   */
  void*
  CTokenTurbo::read_rw(STM_READ_SIG(addr,mask))
  {
      TxThread* tx = stm::Self;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      void* tmp = *addr;
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      CFENCE; // RBR between dereference and orec check

      // get the orec addr, read the orec's version#
      orec_t* o = get_orec(addr);
      uintptr_t ivt = o->v.all;
      // abort if this changed since the last time I saw someone finish
      if (ivt > tx->ts_cache)
          tx->tmabort();

      // log orec
      tx->r_orecs.insert(o);

      // validate, and if we have writes, then maybe switch to fast mode
      if (last_complete.val > tx->ts_cache)
          validate(tx, last_complete.val);
      return tmp;
  }

  /**
   *  CTokenTurbo read (read-turbo mode)
   */
  void*
  CTokenTurbo::read_turbo(STM_READ_SIG(addr,))
  {
      return *addr;
  }

  /**
   *  CTokenTurbo write (read-only context)
   */
  void
  CTokenTurbo::write_ro(STM_WRITE_SIG(addr,val,mask))
  {
      TxThread* tx = stm::Self;
      // we don't have any writes yet, so we need to get an order here
      tx->order = 1 + faiptr(&timestamp.val);

      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));

      stm::OnFirstWrite(read_rw, write_rw, commit_rw);

      // go turbo?
      //
      // NB: we test this on first write, but not subsequent writes, because up
      //     until now we didn't have an order, and thus weren't allowed to use
      //     turbo mode
      validate(tx, last_complete.val);
  }

  /**
   *  CTokenTurbo write (writing context)
   */
  void
  CTokenTurbo::write_rw(STM_WRITE_SIG(addr,val,mask))
  {
      TxThread* tx = stm::Self;
      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  CTokenTurbo write (turbo mode)
   */
  void
  CTokenTurbo::write_turbo(STM_WRITE_SIG(addr,val,mask))
  {
      TxThread* tx = stm::Self;
      // mark the orec, then update the location
      orec_t* o = get_orec(addr);
      o->v.all = tx->order;
      CFENCE;
      STM_DO_MASKED_WRITE(addr, val, mask);
  }

  /**
   *  CTokenTurbo unwinder:
   *
   *    NB: self-aborts in Turbo Mode are not supported.  We could add undo
   *        logging to address this, and add it in Pipeline too.
   */
  void
  CTokenTurbo::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);
      // we cannot be in turbo mode
      if (stm::CheckTurboMode(read_turbo))
          UNRECOVERABLE("Attempting to abort a turbo-mode transaction!");

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      tx->r_orecs.reset();
      tx->writes.reset();
      // NB: we can't reset pointers here, because if the transaction
      //     performed some writes, then it has an order.  If it has an
      //     order, but restarts and is read-only, then it still must call
      //     commit_rw to finish in-order
      PostRollback(tx);
  }

  /**
   *  CTokenTurbo in-flight irrevocability:
   */
  bool CTokenTurbo::irrevoc(TxThread*)
  {
      UNRECOVERABLE("CTokenTurbo Irrevocability not yet supported");
      return false;
  }

  /**
   *  CTokenTurbo validation
   */
  void CTokenTurbo::validate(TxThread* tx, uintptr_t finish_cache)
  {
      // [mfs] There is a performance bug here: we should be looking at the
      //       ts_cache to know if we even need to do this loop.  Consider
      //       single-threaded code: it does a write, it goes to this code, and
      //       then it validates even though it doesn't need to validate, ever!

      if (last_complete.val > tx->ts_cache)
          // [mfs] consider using Luke's trick here
          foreach (OrecList, i, tx->r_orecs) {
              // read this orec
              uintptr_t ivt = (*i)->v.all;
              // if it has a timestamp of ts_cache or greater, abort
              if (ivt > tx->ts_cache)
                  tx->tmabort();
          }

      // now update the finish_cache to remember that at this time, we were
      // still valid
      tx->ts_cache = finish_cache;

      // [mfs] End performance concern

      // and if we are now the oldest thread, transition to fast mode
      if (tx->ts_cache == ((uintptr_t)tx->order - 1)) {
          if (tx->writes.size() != 0) {
              // mark every location in the write set, and perform write-back
              foreach (WriteSet, i, tx->writes) {
                  orec_t* o = get_orec(i->addr);
                  o->v.all = tx->order;
                  CFENCE; // WBW
                  *i->addr = i->val;
              }
              stm::GoTurbo(read_turbo, write_turbo, commit_turbo);
          }
      }
  }

  /**
   *  Switch to CTokenTurbo:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   *
   *    Also, last_complete must equal timestamp
   *
   */
  void
  CTokenTurbo::onSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
      last_complete.val = timestamp.val;
  }
}

namespace stm {
  /**
   *  CTokenTurbo initialization
   */
  template<>
  void initTM<CTokenTurbo>()
  {
      // set the name
      stms[CTokenTurbo].name      = "CTokenTurbo";

      // set the pointers
      stms[CTokenTurbo].begin     = ::CTokenTurbo::begin;
      stms[CTokenTurbo].commit    = ::CTokenTurbo::commit_ro;
      stms[CTokenTurbo].read      = ::CTokenTurbo::read_ro;
      stms[CTokenTurbo].write     = ::CTokenTurbo::write_ro;
      stms[CTokenTurbo].rollback  = ::CTokenTurbo::rollback;
      stms[CTokenTurbo].irrevoc   = ::CTokenTurbo::irrevoc;
      stms[CTokenTurbo].switcher  = ::CTokenTurbo::onSwitchTo;
      stms[CTokenTurbo].privatization_safe = true;
  }
}
