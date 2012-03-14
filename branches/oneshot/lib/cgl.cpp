/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#include <stdint.h>
#include <iostream>
#include <cstdlib>
#include "../common/platform.hpp"
#include "../common/locks.hpp"

namespace stm
{
  static const int MAX_THREADS = 256;

  /**
   *  Padded wrapper around a word to prevent false sharing
   */
  struct pad_word_t
  {
      volatile uintptr_t val;
      char pad[CACHELINE_BYTES-sizeof(uintptr_t)];
  } TM_ALIGN(64);


  /**
   *  Store per-thread metadata.  There isn't much for CGL...
   */
  struct TX
  {
      /*** for flat nesting ***/
      int nesting_depth;

      /*** unique id for this thread ***/
      int id;

      /*** number of commits ***/
      int commits;

      /*** constructor ***/
      TX();
  };

  /**
   *  Array of all threads
   */
  TX* threads[MAX_THREADS];

  /*** Count of all threads ***/
  pad_word_t threadcount = {0};

  /**
   *  Thread-local pointer to self
   */
  __thread TX* Self = NULL;

  /**
   *  Simple constructor for TX: zero all fields, get an ID
   */
  TX::TX() : nesting_depth(0), commits(0)
  {
      id = faiptr(&threadcount.val);
      threads[id] = this;
  }

  /*** The only metadata we need is a single global padded lock ***/
  pad_word_t timestamp = {0};

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
      while (!bcas32(&mtx, 0u, 1u)) { }
      for (uint32_t i = 0; i < threadcount.val; i++) {
          std::cout << "Thread: "    << threads[i]->id
                    << "; Commits: " << threads[i]->commits
                    << std::endl;
      }
      CFENCE;
      mtx = 0;
  }

  /**
   *  For querying to get the current algorithm name
   */
  const char* tm_getalgname() { return "CGL"; }

  /**
   *  Start a transaction: if we're already in a tx, bump the nesting
   *  counter.  Otherwise, grab the lock.
   */
  void tm_begin()
  {
      TX* tx = Self;
      if (++tx->nesting_depth > 1)
          return;
      tatas_acquire(&timestamp.val);
  }

  /**
   *  End a transaction: decrease the nesting level, then perhaps release the
   *  lock and increment the count of commits.
   */
  void tm_end()
  {
      TX* tx = Self;
      if (--tx->nesting_depth)
          return;
      tatas_release(&timestamp.val);
      ++tx->commits;
  }

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
   *  In CGL, malloc doesn't need any special care
   */
  void* tm_alloc(size_t s) { return malloc(s); }

  /**
   *  In CGL, free doesn't need any special care
   */
  void tm_free(void* p) { free(p); }

  /**
   * Legacy stuff, may need eventually
   */
#if 0
  /**
   *  CGL read
   */
  void* CGL::read(STM_READ_SIG(,addr,))
  {
      return *addr;
  }

  /**
   *  CGL write
   */
  void CGL::write(STM_WRITE_SIG(,addr,val,mask))
  {
      STM_DO_MASKED_WRITE(addr, val, mask);
  }
#endif

}
