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
 *  FastlaneSwitch Implementation
 *
 *  Based on J.Wamhoff et.al's paper "FASTLANE: Streamlining Transactions
 *  For Low Thread Counts", TRANSACT'12, FEB.2012, Except that this version
 *  supports master-switching.
 */

#include "../profiling.hpp"
#include "algs.hpp"
#include "RedoRAWUtils.hpp"

// define atomic operations
#define ADD __sync_add_and_fetch
#define SUB __sync_sub_and_fetch
#define OR  __sync_or_and_fetch

using stm::TxThread;
using stm::WriteSet;
using stm::OrecList;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::orec_t;
using stm::get_orec;

// Choose your commit implementation, according to the paper, OPT2 is better
#define OPT1
//#define OPT2

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  static const uint32_t MSB = 0x80000000; // Most Important Bit of 32bit interger
  volatile uint32_t cntr = 0;             // Global counter
  volatile uint32_t helper = 0;           // Helper lock
  volatile uint32_t master = 0;           // Master lock

  struct FastlaneSwitch {
      static TM_FASTCALL bool begin(TxThread*);
      static TM_FASTCALL void* read_ro(STM_READ_SIG(,,));
      static TM_FASTCALL void* read_rw(STM_READ_SIG(,,));
      static TM_FASTCALL void* read_master(STM_READ_SIG(,,));
      static TM_FASTCALL void write_ro(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void write_rw(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void write_master(STM_WRITE_SIG(,,,));
      static TM_FASTCALL void commit_ro(TxThread* tx);
      static TM_FASTCALL void commit_rw(TxThread* tx);
      static TM_FASTCALL void commit_master(TxThread* tx);

      static stm::scope_t* rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
      static NOINLINE bool validate(TxThread* tx);
      static NOINLINE uint32_t WaitForEvenCounter();
      static NOINLINE void EmitWriteSet(TxThread* tx, uint32_t version);
  };

  /**
   *  FastlaneSwitch begin:
   *  Master thread set cntr from even to odd.
   */
  bool
  FastlaneSwitch::begin(TxThread* tx)
  {
      // starts
      tx->allocator.onTxBegin();

      // Acquire master lock to become master
      if (bcas32(&master, 0, 1)) {
          // master request priority access
          OR(&cntr, MSB);

          // Wait for committing helpers
          while ((cntr & 0x01) != 0)
              spin64();

          // Imcrement cntr from even to odd
          cntr = (cntr & ~MSB) + 1;
          WBR;

          // go master mode
          GoTurbo (tx, read_master, write_master, commit_master);
          return true;
      }

      // helpers get even counter (discard LSD & MSB)
      tx->start_time = cntr & ~1 & ~MSB;

      // Go helper mode
      GoTurbo (tx, read_ro, write_ro, commit_ro);

      return true;
  }

  /**
   *  Fastline: commit_master:
   */
  void
  FastlaneSwitch::commit_master(TxThread* tx)
  {
      CFENCE; //wbw between write back and change of cntr
      // Only master can write odd cntr, now cntr is even again
      cntr ++;
      WBR;

      // release master lock
      master = 0;
      WBR;
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  FastlaneSwitch commit (read-only):
   *  Read-only transaction commit immediately
   */
  void
  FastlaneSwitch::commit_ro(TxThread* tx)
  {
      // clean up
      tx->r_orecs.reset();

      // set myself done
      OnReadOnlyCommit(tx);
  }

  /**
   *  FastlaneSwitch commit (writing context):
   *
   */
  void
  FastlaneSwitch::commit_rw(TxThread* tx)
  {
      volatile uint32_t c;

#ifdef OPT1
      // Try to acquiring counter
      // Attempt to CAS only after counter seen even
      do {
          c = WaitForEvenCounter();
      }while (!bcas32(&cntr, c, c+1));

      // Release counter upon failed validation
      if (!validate(tx)) {
          SUB(&cntr, 1);
          tx->tmabort(tx);
      }

      // [NB] Problem: cntr may be negative number now.
      // in the paper, write orec as cntr, which is wrong,
      // should be c+1

      // Write updates to memory, mark orec as c+1
      EmitWriteSet(tx, c+1);

      // Release counter by making it even again
      ADD(&cntr, 1);
#endif

#ifdef OPT2
      // Only one helper at a time (FIFO lock)
      while (!bcas32(&helper, 0, 1));

      c = WaitForEvenCounter();
      // Pre-validate before acquiring counter
      if (!validate(tx)) {
          CFENCE;
          // Release lock upon failed validation
          helper = 0;
          tx->tmabort(tx);
      }
      // Remember validation time
      uint32_t t = c + 1;

      // Likely commit: try acquiring counter
      while (!bcas32(&cntr, c, c + 1))
          c = WaitForEvenCounter();

      // Check that validation still holds
      if (cntr > t && !validate(tx)) {
          // Release locks upon failed validation
          SUB(&cntr, 1);
          helper = 0;
          tx->tmabort(tx);
      }

      // Write updates to memory
      EmitWriteSet(tx, c+1);
      // Release locks
      ADD(&cntr, 1);
      helper = 0;
#endif
      // commit all frees, reset all lists
      tx->r_orecs.reset();
      tx->writes.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  FastlaneSwitch read_master
   */
  void*
  FastlaneSwitch::read_master(STM_READ_SIG(tx,addr,))
  {
      return *addr;
  }

  /**
   *  FastlaneSwitch read (read-only transaction)
   */
  void*
  FastlaneSwitch::read_ro(STM_READ_SIG(tx,addr,))
  {
      void *val = *addr;
      CFENCE;
      // get orec
      orec_t *o = get_orec(addr);

      // validate read value
      if (o->v.all > tx->start_time)
          tx->tmabort(tx);

      // log orec
      tx->r_orecs.insert(o);
      CFENCE;

      // possibly validate before returning
      foreach(OrecList, i, tx->r_orecs) {
          uintptr_t ivt_inner = (*i)->v.all;
          if (ivt_inner > tx->start_time)
              tx->tmabort(tx);
      }
      return val;
  }

  /**
   *  FastlaneSwitch read (writing transaction)
   */
  void*
  FastlaneSwitch::read_rw(STM_READ_SIG(tx,addr,mask))
  {
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // reuse read_ro barrier
      void* val = read_ro(tx, addr STM_MASK(mask));
      REDO_RAW_CLEANUP(val, found, log, mask);
      return val;
  }

  /**
   *  FastlaneSwitch write_master (in place write)
   */
  void
  FastlaneSwitch::write_master(STM_WRITE_SIG(tx,addr,val,mask))
  {
      orec_t* o = get_orec(addr);
      o->v.all = cntr; // mark orec
      CFENCE;
      *addr = val; // in place write
  }

  /**
   *  FastlaneSwitch write (read-only context): for first write
   */
  void
  FastlaneSwitch::write_ro(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // get orec
      orec_t *o = get_orec(addr);
      // validate
      if (o->v.all > tx->start_time)
          tx->tmabort(tx);

      // Add to write set
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  FastlaneSwitch write (writing context)
   */
  void
  FastlaneSwitch::write_rw(STM_WRITE_SIG(tx,addr,val,mask))
  {
      // get orec
      orec_t *o = get_orec(addr);
      // validate
      if (o->v.all > tx->start_time)
          tx->tmabort(tx);

      // record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  FastlaneSwitch unwinder:
   */
  stm::scope_t*
  FastlaneSwitch::rollback(STM_ROLLBACK_SIG(tx, except, len))
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
   *  FastlaneSwitch in-flight irrevocability:
   */
  bool
  FastlaneSwitch::irrevoc(TxThread*)
  {
      UNRECOVERABLE("FastlaneSwitch Irrevocability not yet supported");
      return false;
  }

  /**
   *  FastlaneSwitch validation for commit:
   *  check that all reads and writes are valid
   */
  bool
  FastlaneSwitch::validate(TxThread* tx)
  {
      // check reads
      foreach (OrecList, i, tx->r_orecs){
          // If orec changed , return false
          if ((*i)->v.all > tx->start_time) {
              return false;
          }
      }

      // check writes
      foreach (WriteSet, j, tx->writes) {
          // get orec
          orec_t* o = get_orec(j->addr);
          // If orec changed , return false
          if (o->v.all > tx->start_time) {
              return false;
          }
      }
      return true;
  }

  /**
   *  FastlaneSwitch helper function: wait for even counter
   */
  uint32_t
  FastlaneSwitch::WaitForEvenCounter()
  {
      uint32_t c;
      do {
          c = cntr;
      }while((c & 0x01) != 0);
      return (c & ~MSB);
  }

  /**
   *  FastlaneSwitch helper function: Emit WriteSet
   */
  void
  FastlaneSwitch::EmitWriteSet(TxThread* tx, uint32_t version)
  {
      foreach (WriteSet, i, tx->writes) {
          // get orec
          orec_t* o = get_orec(i->addr);
          // mark orec
          o->v.all = version;
          CFENCE;
          // do write back
          *i->addr = i->val;
      }
  }

  /**
   *  Switch to FastlaneSwitch:
   *
   */
  void
  FastlaneSwitch::onSwitchTo()
  {
      cntr = 0;
  }
}

namespace stm {
  /**
   *  FastlaneSwitch initialization
   */
  template<>
  void initTM<FastlaneSwitch>()
  {
      // set the name
      stms[FastlaneSwitch].name      = "FastlaneSwitch";
      // set the pointers
      stms[FastlaneSwitch].begin     = ::FastlaneSwitch::begin;
      stms[FastlaneSwitch].commit    = ::FastlaneSwitch::commit_ro;
      stms[FastlaneSwitch].read      = ::FastlaneSwitch::read_ro;
      stms[FastlaneSwitch].write     = ::FastlaneSwitch::write_ro;
      stms[FastlaneSwitch].rollback  = ::FastlaneSwitch::rollback;
      stms[FastlaneSwitch].irrevoc   = ::FastlaneSwitch::irrevoc;
      stms[FastlaneSwitch].switcher  = ::FastlaneSwitch::onSwitchTo;
      stms[FastlaneSwitch].privatization_safe = true;
  }
}

