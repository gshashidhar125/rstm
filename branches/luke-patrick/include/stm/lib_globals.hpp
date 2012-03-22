/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

#ifndef LIB_GLOBALS_HPP
#define LIB_GLOBALS_HPP

/**
 *  In this file, we declare functions and variables that need to be visible
 *  to many parts of the STM library.
 */

#ifdef __cplusplus
extern "C" void stm_restart(void) __attribute__((transaction_pure));
extern "C" void stm_enter_waiver(void);
extern "C" void stm_leave_waiver(void);
#else
extern void stm_restart(void) __attribute__((transaction_pure));
extern void stm_enter_waiver(void);
extern void stm_leave_waiver(void);
#endif

#ifdef __cplusplus
#include <stm/config.h>
#include <stm/metadata.hpp>

namespace stm
{
  struct TxThread;
  typedef void (*AbortHandler)(TxThread*);
  void sys_init(AbortHandler conflict_abort);
  void set_policy(const char* phasename);
  void sys_shutdown();
  bool is_irrevoc(const TxThread&);
  void become_irrevoc();
  void restart();
  const char* get_algname();

  extern pad_word_t  threadcount;           // threads in system
  extern TxThread*   threads[MAX_THREADS];  // all TxThreads
}

// used for sandboxing support
extern "C" void stm_validation_full();
#endif
#endif // LIB_GLOBALS_HPP
