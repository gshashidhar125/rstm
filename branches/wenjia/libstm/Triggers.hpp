/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#ifndef TRIGGERS_HPP__
#define TRIGGERS_HPP__

#include "txthread.hpp"
#include "policies.hpp"

namespace stm
{
  /**
   *  This is the code for deciding whether to adapt or not.  It's a little bit
   *  messy because we want to limit what gets inlined.
   */

  /**
   *  part 1: the thing that never gets inlined, and only gets called if we are
   *  definitely going to adapt
   *
   *  [mfs] bad name?
   */
  void trigger_common(TxThread* tx) TM_FASTCALL NOINLINE;

  /**
   * [mfs] Move this stuff to a triggers file?  We might want each STM_XYZ
   *       define to correspond directly to a single file of the same name...
   */

  /**
   *  A simple trigger: request collection of profiles after 16 consecutive
   *  aborts, or on a begin-time wait of >=2048
   */
  struct AbortWaitTrigger
  {
      /**
       *  Part 2: the thing that gets inlined into commit for STMs that have
       *  long blocking at begin time (TML, CGL, Serial, MCS, Ticket), and gets
       *  called on every commit
       */
      inline static void onCommitLock(TxThread* tx)
      {
          // if we don't have a function for changing algs, then we should just
          // return
          if (!pols[curr_policy.POL_ID].decider)
              return;
          // return if we didn't wait long enough
          if (tx->begin_wait <= (unsigned)curr_policy.waitThresh)
              return;
          // ok, we're going to adapt.  Call the common adapt code
          trigger_common(tx);
      }

      /**
       *  This trigger does nothing when an STM transaction commits
       */
      static void onCommitSTM(TxThread*) { }

      /**
       *  Part 3: the thing that gets inlined into stm abort, and gets called
       *  on every abort
       */
      inline static void onAbort(TxThread* tx)
      {
          // if we don't have a function for changing algs, then we should just
          // return
          if (!pols[curr_policy.POL_ID].decider)
              return;
          // return if we didn't abort enough
          if (tx->consec_aborts <= (unsigned)curr_policy.abortThresh)
              return;
          // ok, we're going to adapt.  Call the common adapt code
          curr_policy.abort_switch = true;
          trigger_common(tx);
      }
  };

  /**
   *  This trigger does nothing, and is only around as a baseline
   */
  struct EmptyTrigger
  {
      static void onCommitLock(TxThread*) { }
      static void onCommitSTM(TxThread*) { }
      static void onAbort(TxThread*) { }
  };

  /**
   *  This is the trigger we are currently favoring: request collection of
   *  profiles on 16 consecutive aborts, or when thread 2's commit count
   *  matches an exponentially decaying pattern.
   */
  struct CommitTrigger
  {
      /**
       *  Track the next time to run a trigger.  "time" means the next number
       *  of commits in thread2 that necessitates a trigger.
       */
      static unsigned next;

      /***  Instead of looking at delays, we just count commits */
      static void onCommitLock(TxThread* tx) { onCommitSTM(tx); }

      /*** Count commits to decide if we should request a new profile */
      static void onCommitSTM(TxThread* tx)
      {
          // if we don't have a function for changing algs, then we should just
          // return
          if (!pols[curr_policy.POL_ID].decider)
              return;
          // figure out if this policy likes to move into TML on consec RO
          if (tx->consec_ro > pols[curr_policy.POL_ID].roThresh) {
              curr_policy.abort_switch = false;
              curr_policy.requested_switch = true;
              trigger_common(tx);
              return;
          }
          // return if this policy doesn't allow commit-time probing
          if (!pols[curr_policy.POL_ID].isCommitProfile)
              return;
          // return if not thread#2
          if (tx->id != 2)
              return;
          // return if not a trigger commit number
          unsigned c = tx->num_ro + tx->num_commits;
          if (c != next)
              return;
          // update the trigger commit number
          if (next < 65536)
              next = next * 16;
          else if (next < 524288)
              next += 65536;
          else
              next += 524288;

          // record that this is a non-abort trigger, and call the adapt code
          //
          // NB: this write could be racy if another thread is aborting at the
          //     same time.  In that case, both threads are trying to request
          //     collection of profiles, so it really doesn't matter.
          curr_policy.abort_switch = false;
          // ok, we're going to adapt.  Call the common adapt code
          trigger_common(tx);
      }

      /**
       *  Part 3: the thing that gets inlined into stm abort, and gets called
       *  on every abort.  It's no longer the same as AbortWaitTrigger,
       *  because we need to have extra code to detect when the change is due
       *  to an explicit requested switch.  In other words, if an algorithm
       *  self-detects itself as bad, we want to set a flag in this code.
       */
      static void onAbort(TxThread* tx)
      {
          // if we don't have a function for changing algs, then we should just
          // return
          if (!pols[curr_policy.POL_ID].decider)
              return;
          // return if we didn't abort enough
          if (tx->consec_aborts <= (unsigned)curr_policy.abortThresh)
              return;
          // if the thread's current abort count is HUGE, it means this was a
          // requested abort
          if (tx->consec_aborts > 1024)
              curr_policy.requested_switch = true;
          // ok, we're going to adapt.  Call the common adapt code
          curr_policy.abort_switch = true;
          trigger_common(tx);
      }
  };

#ifdef STM_PROFILETMTRIGGER_ALL
  typedef CommitTrigger Trigger;
#elif defined(STM_PROFILETMTRIGGER_PATHOLOGY)
  typedef AbortWaitTrigger Trigger;
#elif defined(STM_PROFILETMTRIGGER_NONE)
  typedef EmptyTrigger Trigger;
#endif

} // namespace stm

#endif
