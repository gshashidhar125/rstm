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
 *  LLT Implementation
 *
 *    This STM very closely resembles the GV1 variant of TL2.  That is, it uses
 *    orecs and lazy acquire.  Its clock requires everyone to increment it to
 *    commit writes, but this allows for read-set validation to be skipped at
 *    commit time.  Most importantly, there is no in-flight validation: if a
 *    timestamp is greater than when the transaction sampled the clock at begin
 *    time, the transaction aborts.
 */

#include <iostream>
#include <setjmp.h>
#include "MiniVector.hpp"
#include "metadata.hpp"
#include "WriteSet.hpp"
#include "WBMMPolicy.hpp"
#include "Macros.hpp"

namespace stm
{
  typedef MiniVector<orec_t*>      OrecList;     // vector of orecs

  /**
   *  Store per-thread metadata.  There isn't much for CGL...
   */
  struct TX
  {
      /*** for flat nesting ***/
      int nesting_depth;

      /*** unique id for this thread ***/
      int id;

      /*** number of RO commits ***/
      int commits_ro;

      /*** number of RW commits ***/
      int commits_rw;

      id_version_t   my_lock;       // lock word for orec STMs

      int aborts;

      scope_t* volatile scope;      // used to roll back; also flag for isTxnl

      WriteSet       writes;        // write set
      WBMMPolicy     allocator;     // buffer malloc/free
      uintptr_t      start_time;    // start time of transaction
      OrecList       r_orecs;       // read set for orec STMs
      OrecList       locks;         // list of all locks held by tx

      /*** constructor ***/
      TX();
  };

  /**
   *  Simple constructor for TX: zero all fields, get an ID
   */
  TX::TX() : nesting_depth(0), commits_ro(0), commits_rw(0), r_orecs(64), locks(64),
             aborts(0), scope(NULL), writes(64), allocator()
  {
      id = faiptr(&threadcount.val);
      threads[id] = this;
      allocator.setID(id-1);
      // set up my lock word
      my_lock.fields.lock = 1;
      my_lock.fields.id = id;
  }

  /**
   *  No system initialization is required, since the timestamp is already 0
   */
  void tm_sys_init() { }

  /**
   *  When the transactional system gets shut down, we call this to dump
   *  stats for all threads
   */
  void tm_sys_shutdown()
  {
      static volatile unsigned int mtx = 0;
      // while (!bcas32(&mtx, 0u, 1u)) { }
      for (uint32_t i = 0; i < threadcount.val; i++) {
          std::cout << "Thread: "       << threads[i]->id
                    << "; RO Commits: " << threads[i]->commits_ro
                    << "; RW Commits: " << threads[i]->commits_rw
                    << "; Aborts: "     << threads[i]->aborts
                    << std::endl;
      }
      CFENCE;
      mtx = 0;
  }

  /**
   *  For querying to get the current algorithm name
   */
  const char* tm_getalgname() { return "LLT"; }

  /**
   *  To initialize the thread's TM support, we need only ensure it has a
   *  descriptor.
   */
  void tm_thread_init()
  {
      // multiple inits from one thread do not cause trouble
      if (Self) return;

      // create a TxThread and save it in thread-local storage
      Self = new TX();
  }

  /**
   *  When a thread is done using the TM, we don't need to do anything
   *  special.
   */
  void tm_thread_shutdown() { }

  /**
   *  Abort and roll back the transaction (e.g., on conflict).
   */
  scope_t* rollback(TX* tx)
  {
      ++tx->aborts;

      // release the locks and restore version numbers
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = (*i)->p;

      // undo memory operations, reset lists
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      tx->allocator.onTxAbort();
      tx->nesting_depth = 0;
      scope_t* scope = tx->scope;
      tx->scope = NULL;
      return scope;
  }

  /**
   *  The default mechanism that libstm uses for an abort. An API environment
   *  may also provide its own abort mechanism (see itm2stm for an example of
   *  how the itm shim does this).
   *
   *  This is ugly because rollback has a configuration-dependent signature.
   */
  NOINLINE
  NORETURN
  void tm_abort(TX* tx)
  {
      jmp_buf* scope = (jmp_buf*)rollback(tx);
      // need to null out the scope
      longjmp(*scope, 1);
  }

  /*** The only metadata we need is a single global padded lock ***/
  pad_word_t timestamp = {0};

  /**
   *  LLT begin:
   */
  void tm_begin(scope_t* scope)
  {
      TX* tx = Self;
      if (++tx->nesting_depth > 1)
          return;

      tx->scope = scope;

      tx->allocator.onTxBegin();
      // get a start time
      tx->start_time = timestamp.val;
  }

  /**
   *  LLT validation
   */
  NOINLINE
  void validate(TX* tx)
  {
      // validate
      foreach (OrecList, i, tx->r_orecs) {
          uintptr_t ivt = (*i)->v.all;
          // if unlocked and newer than start time, abort
          if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
              tm_abort(tx);
      }
  }

  /**
   *  LLT commit (read-only):
   */
  void tm_end()
  {
      TX* tx = Self;
      if (--tx->nesting_depth)
          return;

      if (!tx->writes.size()) {
          // read-only, so just reset lists
          tx->r_orecs.reset();
          tx->allocator.onTxCommit();
          ++tx->commits_ro;
          return;
      }

      // acquire locks
      foreach (WriteSet, i, tx->writes) {
          // get orec, read its version#
          orec_t* o = get_orec(i->addr);
          uintptr_t ivt = o->v.all;

          // lock all orecs, unless already locked
          if (ivt <= tx->start_time) {
              // abort if cannot acquire
              if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                  tm_abort(tx);
              // save old version to o->p, remember that we hold the lock
              o->p = ivt;
              tx->locks.insert(o);
          }
          // else if we don't hold the lock abort
          else if (ivt != tx->my_lock.all) {
              tm_abort(tx);
          }
      }

      // increment the global timestamp since we have writes
      uintptr_t end_time = 1 + faiptr(&timestamp.val);

      // skip validation if nobody else committed
      if (end_time != (tx->start_time + 1))
          validate(tx);

      // run the redo log
      tx->writes.writeback();

      // release locks
      CFENCE;
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = end_time;

      // clean-up
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();

      tx->allocator.onTxCommit();
      ++tx->commits_rw;
  }

  /**
   *  LLT read (read-only transaction)
   *
   *    We use "check twice" timestamps in LLT
   */
  void* tm_read(void** addr)
  {
      TX* tx = Self;

      if (tx->writes.size()) {
          // check the log for a RAW hazard, we expect to miss
          WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
          bool found = tx->writes.find(log);
          if (found)
              return log.val;
      }

      // get the orec addr
      orec_t* o = get_orec(addr);

      // read orec, then val, then orec
      uintptr_t ivt = o->v.all;
      CFENCE;
      void* tmp = *addr;
      CFENCE;
      uintptr_t ivt2 = o->v.all;
      // if orec never changed, and isn't too new, the read is valid
      if ((ivt <= tx->start_time) && (ivt == ivt2)) {
          // log orec, return the value
          tx->r_orecs.insert(o);
          return tmp;
      }
      // unreachable
      tm_abort(tx);
      return NULL;
  }

  /**
   *  LLT write (read-only context)
   */
  void tm_write(void** addr, void* val)
  {
      TX* tx = Self;
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  get a chunk of memory that will be automatically reclaimed if the caller
   *  is a transaction that ultimately aborts
   */
  void* tm_alloc(size_t size) { return Self->allocator.txAlloc(size); }

  /**
   *  Free some memory.  If the caller is a transaction that ultimately aborts,
   *  the free will not happen.  If the caller is a transaction that commits,
   *  the free will happen at commit time.
   */
  void tm_free(void* p) { Self->allocator.txFree(p); }

}
