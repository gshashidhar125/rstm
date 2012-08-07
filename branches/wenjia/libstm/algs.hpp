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
 *  This file declares global metadata that is used by all STM algorithms,
 *  along with some accessor functions
 */

#ifndef ALGS_HPP__
#define ALGS_HPP__

#ifdef STM_CC_SUN
#include <stdio.h>
#else
#include <cstdio>
#endif

#include "metadata.hpp"
#include "txthread.hpp"
#include "profiling.hpp" // Trigger::
#include <algnames-autogen.hpp> // defines the ALGS enum
#include "../include/abstract_timing.hpp"

// [mfs] this isn't the right place for these defines, but they help to
//       reduce code size and the prominence of this placement will hopefully
//       lead to it being cleaned up properly soon...
#define COHORTS_COMMITTED 0
#define COHORTS_STARTED   1
#define COHORTS_CPENDING  2
#define COHORTS_NOTDONE   3
#define COHORTS_DONE      4
const uintptr_t VALIDATION_FAILED = 1;

namespace stm
{
  /**
   *  These constants are used throughout the STM implementations
   */
  static const uint32_t NUM_STRIPES   = 1048576;  // number of orecs
  static const uint32_t RING_ELEMENTS = 1024;     // number of ring elements
  static const uint32_t KARMA_FACTOR  = 16;       // aborts b4 incr karma
  static const uint32_t BACKOFF_MIN   = 4;        // min backoff exponent
  static const uint32_t BACKOFF_MAX   = 16;       // max backoff exponent
  static const uint32_t RREC_COUNT    = 1048576;  // number of rrecs
  static const uint32_t WB_CHUNK_SIZE = 16;       // lf writeback chunks
  static const uint32_t EPOCH_MAX     = INT_MAX;  // default epoch
  static const uint32_t ACTIVE        = 0;        // transaction status
  static const uint32_t ABORTED       = 1;        // transaction status
  static const uint32_t SWISS_PHASE2  = 10;       // swisstm cm phase change thresh

  /**
   *  These global fields are used for concurrency control and conflict
   *  detection in our STM systems
   */
  extern pad_word_t    timestamp;
  extern orec_t        orecs[NUM_STRIPES];             // set of orecs
  extern pad_word_t    last_init;                      // last logical commit
  extern pad_word_t    last_complete;                  // last physical commit
  extern filter_t ring_wf[RING_ELEMENTS] TM_ALIGN(16); // ring of Bloom filters
  extern pad_word_t    prioTxCount;                    // # priority txns
  extern rrec_t        rrecs[RREC_COUNT];              // set of rrecs
  extern bytelock_t    bytelocks[NUM_STRIPES];         // set of bytelocks
  extern bitlock_t     bitlocks[NUM_STRIPES];          // set of bitlocks
  extern pad_word_t    timestamp_max;                  // max value of timestamp
  extern mcs_qnode_t*  mcslock;                        // for MCS
  extern pad_word_t    epochs[MAX_THREADS];            // for coarse-grained CM
  extern ticket_lock_t ticketlock;                     // for ticket lock STM
  extern orec_t        nanorecs[RING_ELEMENTS];        // for Nano
  extern pad_word_t    greedy_ts;                      // for swiss cm
  extern pad_word_t    fcm_timestamp;                  // for FCM
  extern dynprof_t*    app_profiles;                   // for ProfileApp*

  // ProfileTM can't function without these
  extern dynprof_t*    profiles;          // a list of ProfileTM measurements
  extern uint32_t      profile_txns;      // how many txns per profile

  // Global variables for Cohorts
  extern volatile uint32_t locks[9];  // a big lock at locks[0], and
                                      // small locks from locks[1] to locks[8]
  extern  pad_word_t started;         // number of tx started
  extern  pad_word_t cpending;        // number of tx waiting to commit
  extern  pad_word_t committed;       // number of tx committed
  extern volatile int32_t last_order; // order of last tx in a cohort + 1
  extern volatile uint32_t gatekeeper;// indicating whether tx can start
  extern filter_t* global_filter;     // global filter
  extern filter_t* temp_filter;       // temp filter
  extern AddressList addrs;           // temp address list

  // Global variables for Fastlane
  extern pad_word_t helper;

  // Global variables for PTM
  extern pad_word_t global_version;
  extern pad_word_t writer_lock;

  /**
   *  To describe an STM algorithm, we provide a name, a set of function
   *  pointers, and some other information
   */
  struct alg_t
  {
      /*** the name of this policy */
      const char* name;

      /**
       * the begin, commit, read, and write methods a tx uses when it
       * starts
       */
      void  (* begin) ();
      void  (*TM_FASTCALL commit)();
      void* (*TM_FASTCALL read)  (STM_READ_SIG(,));
      void  (*TM_FASTCALL write) (STM_WRITE_SIG(,,));

      /**
       * rolls the transaction back without unwinding, returns the scope (which
       * is set to null during rollback)
       */
      void (* rollback)(STM_ROLLBACK_SIG(,,));

      /*** the restart, retry, and irrevoc methods to use */
      bool  (* irrevoc)(TxThread*);

      /*** the code to run when switching to this alg */
      void  (* switcher) ();

      /**
       *  bool flag to indicate if an algorithm is privatization safe
       *
       *  NB: we should probably track levels of publication safety too, but
       *      we don't
       */
      bool privatization_safe;

      /*** simple ctor, because a NULL name is a bad thing */
      alg_t() : name("") { }
  };

  /**
   *  These simple functions are used for common operations on the global
   *  metadata arrays
   */

  /**
   *  Map addresses to orec table entries
   */
  inline orec_t* get_orec(void* addr)
  {
      uintptr_t index = reinterpret_cast<uintptr_t>(addr);
      return &orecs[(index>>3) % NUM_STRIPES];
  }

  /**
   *  Map addresses to nanorec table entries
   */
  inline orec_t* get_nanorec(void* addr)
  {
      uintptr_t index = reinterpret_cast<uintptr_t>(addr);
      return &nanorecs[(index>>3) % RING_ELEMENTS];
  }

  /**
   *  Map addresses to rrec table entries
   */
  inline rrec_t* get_rrec(void* addr)
  {
      uintptr_t index = reinterpret_cast<uintptr_t>(addr);
      return &rrecs[(index>>3)%RREC_COUNT];
  }

  /**
   *  Map addresses to bytelock table entries
   */
  inline bytelock_t* get_bytelock(void* addr)
  {
      uintptr_t index = reinterpret_cast<uintptr_t>(addr);
      return &bytelocks[(index>>3) % NUM_STRIPES];
  }

  /**
   *  Map addresses to bitlock table entries
   */
  inline bitlock_t* get_bitlock(void* addr)
  {
      uintptr_t index = reinterpret_cast<uintptr_t>(addr);
      return &bitlocks[(index>>3) % NUM_STRIPES];
  }

  /**
   *  We don't want to have to declare an init function for each of the STM
   *  algorithms that exist, because there are very many of them and they vary
   *  dynamically.  Instead, we have a templated init function in namespace stm,
   *  and we instantiate it once per algorithm, in the algorithm's .cpp, using
   *  the ALGS enum.  Then we can just call the templated functions from this
   *  code, and the linker will find the corresponding instantiation.
   */
  template <int I>
  void initTM();

  /**
   *  These describe all our STM algorithms and adaptivity policies
   */
  extern alg_t stms[ALG_MAX];

  /*** Get an ENUM value from a string TM name */
  int32_t stm_name_map(const char*);

  /**
   *  A simple implementation of randomized exponential backoff.
   *
   *  NB: This uses getElapsedTime, which is slow compared to a granularity
   *      of 64 nops.  However, we can't switch to tick(), because sometimes
   *      two successive tick() calls return the same value?
   */
  inline void exp_backoff(TxThread* tx)
  {
      // how many bits should we use to pick an amount of time to wait?
      uint32_t bits = tx->consec_aborts + BACKOFF_MIN - 1;
      bits = (bits > BACKOFF_MAX) ? BACKOFF_MAX : bits;
      // get a random amount of time to wait, bounded by an exponentially
      // increasing limit
      int32_t delay = rand_r_32(&tx->seed);
      delay &= ((1 << bits)-1);
      // wait until at least that many ns have passed
      unsigned long long start = getElapsedTime();
      unsigned long long stop_at = start + delay;
      while (getElapsedTime() < stop_at) { spin64(); }
  }

  // This is used as a default in txthread.cpp... just forwards to CGL::begin.
  void begin_CGL();

  typedef TM_FASTCALL void* (*ReadBarrier)(STM_READ_SIG(,));
  typedef TM_FASTCALL void (*WriteBarrier)(STM_WRITE_SIG(,,));
  typedef TM_FASTCALL void (*CommitBarrier)();

  inline void OnReadWriteCommit(TxThread* tx, ReadBarrier read_ro,
                                WriteBarrier write_ro, CommitBarrier commit_ro)
  {
      tx->allocator.onTxCommit();
      tx->abort_hist.onCommit(tx->consec_aborts);
      tx->consec_aborts = 0;
      tx->consec_ro = 0;
      ++tx->num_commits;
      tmread = read_ro;
      tmwrite = write_ro;
      tmcommit = commit_ro;
      Trigger::onCommitSTM(tx);
  }

  inline void OnReadWriteCommit(TxThread* tx)
  {
      tx->allocator.onTxCommit();
      tx->abort_hist.onCommit(tx->consec_aborts);
      tx->consec_aborts = 0;
      tx->consec_ro = 0;
      ++tx->num_commits;
      Trigger::onCommitSTM(tx);
  }

  inline void OnReadOnlyCommit(TxThread* tx)
  {
      tx->allocator.onTxCommit();
      tx->abort_hist.onCommit(tx->consec_aborts);
      tx->consec_aborts = 0;
      ++tx->consec_ro;
      ++tx->num_ro;
      Trigger::onCommitSTM(tx);
  }

  inline void OnCGLCommit(TxThread* tx)
  {
      tx->allocator.onTxCommitImmediate();
      tx->consec_ro = 0;
      ++tx->num_commits;
      Trigger::onCommitLock(tx);
  }

  inline void OnReadOnlyCGLCommit(TxThread* tx)
  {
      tx->allocator.onTxCommit();
      ++tx->consec_ro;
      ++tx->num_ro;
      Trigger::onCommitLock(tx);
  }

  inline void OnFirstWrite(ReadBarrier read_rw, WriteBarrier write_rw,
                           CommitBarrier commit_rw)
  {
      tmread = read_rw;
      tmwrite = write_rw;
      tmcommit = commit_rw;
  }

  inline void PreRollback(TxThread* tx)
  {
      ++tx->num_aborts;
      ++tx->consec_aborts;
  }

  inline void
  PostRollback(TxThread* tx, ReadBarrier read_ro,
               WriteBarrier write_ro, CommitBarrier commit_ro)
  {
      tx->allocator.onTxAbort();
      tx->nesting_depth = 0;
      tmread = read_ro;
      tmwrite = write_ro;
      tmcommit = commit_ro;
      Trigger::onAbort(tx);
      tx->in_tx = false;
  }

  inline void PostRollback(TxThread* tx)
  {
      tx->allocator.onTxAbort();
      tx->nesting_depth = 0;
      Trigger::onAbort(tx);
      tx->in_tx = false;
  }

  /**
   *  Custom PostRollback code for ProfileTM.  If a transaction other than
   *  the last in the profile set aborts, we roll it back using this
   *  function, which does everything the prior PostRollback did except for
   *  calling the "Trigger::onAbort()" method.
   */
  inline void
  PostRollbackNoTrigger(TxThread* tx, ReadBarrier r,
                        WriteBarrier w, CommitBarrier c)
  {
      tx->allocator.onTxAbort();
      tx->nesting_depth = 0;
      tmread = r;
      tmwrite = w;
      tmcommit = c;
      tx->in_tx = false;
  }

  /**
  *  Custom PostRollback code for ProfileTM.  If the last transaction in the
  *  profile set aborts, it will call profile_oncomplete before calling this.
  *  That means that it will adapt /out of/ ProfileTM, which in turn means
  *  that we cannot reset the pointers on abort.
  */
  inline void PostRollbackNoTrigger(TxThread* tx)
  {
      tx->allocator.onTxAbort();
      tx->nesting_depth = 0;
      tx->in_tx = false;
  }

  inline void GoTurbo(ReadBarrier r, WriteBarrier w, CommitBarrier c)
  {
      tmread = r;
      tmwrite = w;
      tmcommit = c;
  }

  inline bool CheckTurboMode(ReadBarrier read_turbo)
  {
      return (tmread == read_turbo);
  }

  /**
   *  Stuff from metadata.hpp
   */

  /**
   *  Setting the read byte is platform-specific, so we are going to put it
   *  here to avoid lots of ifdefs in many code locations.  The issue is
   *  that we need this write to also be a WBR fence, and the cheapest WBR
   *  is platform-dependent
   */
  inline void bytelock_t::set_read_byte(uint32_t id)
  {
#if defined(STM_CPU_SPARC)
      reader[id] = 1;   WBR;
#else
      atomicswap8(&reader[id], 1u);
#endif
  }

  /*** set a bit */
  inline void rrec_t::setbit(unsigned slot)
  {
      uint32_t bucket = slot / BITS;
      uintptr_t mask = 1lu<<(slot % BITS);
      uintptr_t oldval = bits[bucket];
      if (oldval & mask)
          return;
      while (true) {
          if (bcasptr(&bits[bucket], oldval, (oldval | mask)))
              return;
          oldval = bits[bucket];
      }
  }

  /*** test a bit */
  inline bool rrec_t::getbit(unsigned slot)
  {
      unsigned bucket = slot / BITS;
      uintptr_t mask = 1lu<<(slot % BITS);
      uintptr_t oldval = bits[bucket];
      return oldval & mask;
  }

  /*** unset a bit */
  inline void rrec_t::unsetbit(unsigned slot)
  {
      uint32_t bucket = slot / BITS;
      uintptr_t mask = 1lu<<(slot % BITS);
      uintptr_t unmask = ~mask;
      uintptr_t oldval = bits[bucket];
      if (!(oldval & mask))
          return;
      // NB:  this GCC-specific code
#if defined(STM_CPU_X86) && defined(STM_CC_GCC)
      __sync_fetch_and_and(&bits[bucket], unmask);
#else
      while (true) {
          if (bcasptr(&bits[bucket], oldval, (oldval & unmask)))
              return;
          oldval = bits[bucket];
      }
#endif
  }

  /*** combine test and set */
  inline bool rrec_t::setif(unsigned slot)
  {
      uint32_t bucket = slot / BITS;
      uintptr_t mask = 1lu<<(slot % BITS);
      uintptr_t oldval = bits[bucket];
      if (oldval & mask)
          return false;
      // NB: We don't have suncc fetch_and_or, so there is an ifdef here that
      //     falls back to a costly CAS-based atomic or
#if defined(STM_CPU_X86) && defined(STM_CC_GCC) /* little endian */
      __sync_fetch_and_or(&bits[bucket], mask);
      return true;
#else
      while (true) {
          if (bcasptr(&bits[bucket], oldval, oldval | mask))
              return true;
          oldval = bits[bucket];
      }
#endif
  }

  /*** bitwise or */
  inline void rrec_t::operator |= (rrec_t& rhs)
  {
      // NB: We could probably use SSE here, but since we've only got ~256
      //    bits, the savings would be minimal
      for (unsigned i = 0; i < BUCKETS; ++i)
          bits[i] |= rhs.bits[i];
  }

  /*** on commit, update the appropriate bucket */
  inline void toxic_histogram_t::onCommit(uint32_t aborts)
  {
      if (aborts < 17) {
          buckets[aborts]++;
      }
      // overflow bucket: must also update the max value
      else {
          buckets[17]++;
          if (aborts > max)
              max = aborts;
      }
  }

      /*** simple printout */
  inline void toxic_histogram_t::dump()
  {
      printf("abort_histogram: ");
      for (int i = 0; i < 18; ++i)
          printf("%d, ", buckets[i]);
      printf("max = %d, hgc = %d, hga = %d\n", max, hg_commits, hg_aborts);
  }

  /*** on hourglass commit */
  inline void toxic_histogram_t::onHGCommit() { hg_commits++; }
  inline void toxic_histogram_t::onHGAbort() { hg_aborts++; }

} // namespace stm

#endif // ALGS_HPP__
