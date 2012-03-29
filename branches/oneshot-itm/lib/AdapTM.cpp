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
#include <setjmp.h> // factor this out into the API?
#include "tx.hpp"
#include "platform.hpp"
#include "locks.hpp"
#include "metadata.hpp"
#include "adaptivity.hpp"

using namespace stm;

/**
 * This STM is implemented differently than all others.  We don't give it a
 * custom namespace, and we don't rely on tx.cpp.  Instead, we implement
 * everything directly within this file.  In that way, we can get all of our
 * adaptivity hooks to work correctly.
 *
 * NB: right now, we pick an algorithm at begin time, but we don't actually
 *     adapt yet
 */
namespace stm {
  /**
   *  All behaviors are reached via function pointers.  This allows us to
   *  change on the fly. These are public because there is an api file
   *  (stmapi_fptr) that wants to call them directly.
   */
  rollback_t        rollback_;
  tm_begin_t        tm_begin_;
  tm_end_t          tm_end_;
  tm_get_alg_name_t tm_getalgname_;
  tm_alloc_t        tm_alloc_;
  tm_free_t         tm_free_;
  tm_read_t         tm_read_;
  tm_write_t        tm_write_;

  // forward all calls to the function pointers
  uint32_t tm_begin(uint32_t flags) {
      return tm_begin_(flags);
  }

  void tm_end() {
      tm_end_();
  }

  void* tm_alloc(size_t s) {
      return tm_alloc_(s);
  }

  void tm_free(void* p) {
      tm_free_(p);
  }

  void* TM_FASTCALL tm_read(void** addr) {
      return tm_read_(addr);
  }

  void TM_FASTCALL tm_write(void** addr, void* val) {
      tm_write_(addr, val);
  }

  checkpoint_t* rollback(TX* tx) {
      return rollback_(tx);
  }

  // [ld] why not a static inside of tm_getalgname? Are we avoiding the
  //      thread-safe initialization concern?
  static char* trueAlgName = NULL;
  const char* tm_getalgname() {
      if (trueAlgName)
          return trueAlgName;

      const char* s1 = "AdapTM";
      const char* s2 = tm_getalgname_();
      size_t l1 = strlen(s1);
      size_t l2 = strlen(s2);
      trueAlgName = (char*)malloc((l1+l2+3)*sizeof(char));
      strcpy(trueAlgName, s1);
      trueAlgName[l1] = trueAlgName[l1+1] = ':';
      strcpy(&trueAlgName[l1+2], s2);
      return trueAlgName;
  }

  /**
   *  We don't need, and don't want, to use the REGISTER_TM_FOR_XYZ macros,
   *  but we still need to make sure that there is an initTM<AdapTM>
   *  symbol. This is because the name enum is manually generated.
   */
  template <> void initTM<AdapTM>() {
  }
}

/**
 *  Template Metaprogramming trick for initializing all STM algorithms.
 */
template <int I>
static void init_tm_info() {
    initTM<(TM_NAMES)I>();
    init_tm_info<I-1>();
}

template <>
void init_tm_info<0>() {
    initTM<(TM_NAMES)0>();
}

static void init_tm_info() {
    init_tm_info<TM_NAMES_MAX - 1>();
}

/**
 *  Initialize all of the TM algorithms
 */
static void __attribute((constructor)) library_init() {
    // call of the initTM, to have them register themselves with tm_info
    init_tm_info();

    // guess a default configuration, then check env for a better option
    const char* cfg = "NOrec";
    const char* configstring = getenv("STM_CONFIG");
    if (configstring)
        cfg = configstring;
    else
        printf("STM_CONFIG environment variable not found... using %s\n", cfg);

    bool found = false;
    for (int i = 0; i < TM_NAMES_MAX; ++i) {
        const char* name = tm_info[i].tm_getalgname();
        if (0 == strcmp(cfg, name)) {
            rollback_ = tm_info[i].rollback;
            tm_begin_ = tm_info[i].tm_begin;
            tm_end_ = tm_info[i].tm_end;
            tm_getalgname_ = tm_info[i].tm_getalgname;
            tm_alloc_ = tm_info[i].tm_alloc;
            tm_free_ = tm_info[i].tm_free;
            tm_read_ = tm_info[i].tm_read;
            tm_write_ = tm_info[i].tm_write;
            found = true;
            break;
        }
    }
    printf("STM library configured using config == %s\n", cfg);
}
