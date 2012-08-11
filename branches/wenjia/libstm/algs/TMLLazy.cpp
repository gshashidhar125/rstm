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
 *  TMLLazy Implementation
 *
 *    This is just like TML, except that we use buffered update and we wait to
 *    become the 'exclusive writer' until commit time.  The idea is that this
 *    is supposed to increase concurrency, and also that this should be quite
 *    fast even though it has the function call overhead.  This algorithm
 *    provides at least ALA semantics.
 */

#include "../profiling.hpp"
#include "algs.hpp"
#include "../RedoRAWUtils.hpp"

using stm::TxThread;
using stm::timestamp;
using stm::WriteSetEntry;

/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.  Note that with TML, we don't expect the reads and
 *  writes to be called, because we expect the isntrumentation to be inlined
 *  via the dispatch mechanism.  However, we must provide the code to handle
 *  the uncommon case.
 */
namespace {
  struct TMLLazy {
      static void begin(TX_LONE_PARAMETER);
      static TM_FASTCALL void* read_ro(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_rw(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void commit_ro(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_rw(TX_LONE_PARAMETER);

      static void rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
  };

  /**
   *  TMLLazy begin:
   */
  void TMLLazy::begin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // Sample the sequence lock until it is even (unheld)
      while ((tx->start_time = timestamp.val)&1)
          spin64();

      // notify the allocator
      tx->allocator.onTxBegin();
  }

  /**
   *  TMLLazy commit (read-only context):
   */
  void
  TMLLazy::commit_ro(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // no metadata to manage, so just be done!
      OnROCommit(tx);
  }

  /**
   *  TMLLazy commit (writer context):
   */
  void
  TMLLazy::commit_rw(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // we have writes... if we can't get the lock, abort
      if (!bcasptr(&timestamp.val, tx->start_time, tx->start_time + 1))
          stm::tmabort();

      // we're committed... run the redo log
      tx->writes.writeback();

      // release the sequence lock and clean up
      timestamp.val++;
      tx->writes.reset();
      OnRWCommit(tx);
      ResetToRO(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  TMLLazy read (read-only context)
   */
  void*
  TMLLazy::read_ro(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // read the actual value, direct from memory
      void* tmp = *addr;
      CFENCE;

      // if the lock has changed, we must fail
      //
      // NB: this form of /if/ appears to be faster
      if (__builtin_expect(timestamp.val == tx->start_time, true))
          return tmp;
      stm::tmabort();
      // unreachable
      return NULL;
  }

  /**
   *  TMLLazy read (writing context)
   */
  void*
  TMLLazy::read_rw(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // reuse the ReadRO barrier, which is adequate here---reduces LOC
      void* val = read_ro(TX_FIRST_ARG addr STM_MASK(mask));
      REDO_RAW_CLEANUP(val, found, log, mask);
      return val;
  }

  /**
   *  TMLLazy write (read-only context):
   */
  void
  TMLLazy::write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // do a buffered write
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      stm::OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  TMLLazy write (writing context):
   */
  void
  TMLLazy::write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // do a buffered write
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  TMLLazy unwinder
   */
  void
  TMLLazy::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);
      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      tx->writes.reset();
      PostRollback(tx);
      ResetToRO(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  TMLLazy in-flight irrevocability:
   */
  bool
  TMLLazy::irrevoc(TxThread* tx)
  {
      // we are running in isolation by the time this code is run.  Make sure
      // we are valid.
      if (!bcasptr(&timestamp.val, tx->start_time, tx->start_time + 1))
          return false;

      // push all writes back to memory and clear writeset
      tx->writes.writeback();
      timestamp.val++;

      // return the STM to a state where it can be used after we finish our
      // irrevoc transaction
      tx->writes.reset();
      return true;
  }

  /**
   *  Switch to TMLLazy:
   *
   *    We just need to be sure that the timestamp is not odd
   */
  void
  TMLLazy::onSwitchTo()
  {
      if (timestamp.val & 1)
          ++timestamp.val;
  }
}

namespace stm {
  /**
   *  TMLLazy initialization
   */
  template<>
  void initTM<TMLLazy>()
  {
      // set the name
      stm::stms[TMLLazy].name     = "TMLLazy";

      // set the pointers
      stm::stms[TMLLazy].begin    = ::TMLLazy::begin;
      stm::stms[TMLLazy].commit   = ::TMLLazy::commit_ro;
      stm::stms[TMLLazy].read     = ::TMLLazy::read_ro;
      stm::stms[TMLLazy].write    = ::TMLLazy::write_ro;
      stm::stms[TMLLazy].rollback = ::TMLLazy::rollback;
      stm::stms[TMLLazy].irrevoc  = ::TMLLazy::irrevoc;
      stm::stms[TMLLazy].switcher = ::TMLLazy::onSwitchTo;
      stm::stms[TMLLazy].privatization_safe = true;
  }
}

#ifdef STM_ONESHOT_ALG_TMLLazy
DECLARE_AS_ONESHOT_NORMAL(TMLLazy)
#endif
