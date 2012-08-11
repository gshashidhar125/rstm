/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#ifdef STM_CC_SUN
#include <stdio.h>
#else
#include <cstdio>
#endif

#include "algs.hpp"
#include "cm.hpp"

namespace stm
{
  /*** BACKING FOR GLOBAL METADATA */

  /**
   *  This is the Orec Timestamp, the NOrec/TML seqlock, the CGL lock, and the
   *  RingSW ring index
   */
  pad_word_t timestamp = {0, {0}};

  /**
   *  Sometimes we use the Timestamp not as a counter, but as a bool.  If we
   *  want to switch back to using it as a counter, we need to know what the
   *  old value was.  This holds the old value.
   *
   *  This is only used within STM implementations, to log and recover the
   *  value
   */
  pad_word_t timestamp_max = {0, {0}};

  const uint32_t BACKOFF_MIN   = 4;        // min backoff exponent
  const uint32_t BACKOFF_MAX   = 16;       // max backoff exponent


  /*** the ring */
  pad_word_t last_complete = {0, {0}};
  pad_word_t last_init     = {0, {0}};
  filter_t   ring_wf[RING_ELEMENTS] TM_ALIGN(16);

  /*** priority stuff */
  pad_word_t prioTxCount       = {0, {0}};

  /*** the array of epochs */
  pad_word_t epochs[MAX_THREADS] = {{0, {0}}};

  /*** Swiss greedy CM */
  pad_word_t greedy_ts = {0, {0}};

  /*** for MCS */
  mcs_qnode_t* mcslock = NULL;

  /*** for Ticket */
  ticket_lock_t ticketlock  = {0, 0};

  /*** for some CMs */
  pad_word_t fcm_timestamp = {0, {0}};

  /*** for Cohorts */
  volatile uint32_t locks[9] = {0};
  pad_word_t started = {0, {0}};
  pad_word_t cpending = {0, {0}};
  pad_word_t committed = {0, {0}};
  volatile int32_t last_order = 1;
  volatile uint32_t gatekeeper = 0;
  filter_t* global_filter = (filter_t*)FILTER_ALLOC(sizeof(filter_t));
  filter_t* temp_filter = (filter_t*)FILTER_ALLOC(sizeof(filter_t));
  AddressList addrs = AddressList (64);

  /*** for Fastlane*/
  pad_word_t helper = {0, {0}};

  /*** for PTM*/
  pad_word_t global_version = {1, {0}};
  pad_word_t writer_lock = {0, {0}};

  /*** Store descriptions of the STM algorithms */
  alg_t stms[ALG_MAX];

  /*** for ProfileApp* */
  dynprof_t*   app_profiles       = NULL;

  /***  These are the adaptivity-related fields */
  uint32_t   profile_txns = 1;          // number of txns per profile
  dynprof_t* profiles     = NULL;       // where to store profiles

  /*** Use the stms array to map a string name to an algorithm ID */
  int stm_name_map(const char* phasename)
  {
      for (int i = 0; i < ALG_MAX; ++i)
          if (0 == strcmp(phasename, stms[i].name))
              return i;
      return -1;
  }

  /**
   *  A simple implementation of randomized exponential backoff.
   *
   *  NB: This uses getElapsedTime, which is slow compared to a granularity
   *      of 64 nops.  However, we can't switch to tick(), because sometimes
   *      two successive tick() calls return the same value and tickp isn't
   *      universal.
   */
  void exp_backoff(TxThread* tx)
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

} // namespace stm
