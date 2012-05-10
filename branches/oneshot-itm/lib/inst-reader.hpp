/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */
#ifndef RSTM_INST_RAW_H
#define RSTM_INST_RAW_H

#include "byte-logging.hpp"

/**
 *  This header define the read-afeter-write algorithms used in the read
 *  instrumentation (inst.hpp) as RAW policies.
 */
namespace stm {
  template <class Read, class WordType>
  struct Reader {
      TX* tx;
      Read read;

      Reader(TX* tx) : tx(tx), read() {
      }

      void __attribute__((always_inline))
      operator()(void** address, void*& w, uintptr_t mask) const {
          w = read(address, tx, mask);
      }
  };

  template <class Read>
  struct Reader<Read, Word> {
      TX* tx;
      Read read;

      Reader(TX* tx) : tx(tx), read() {
      }

      void __attribute__((always_inline))
      operator()(void** address, void*& w, uintptr_t mask) const {
          if (!tx->writes.find(address, w))
              w = read(address, tx, mask);
      }
  };

  template <class Read>
  struct Reader<Read, MaskedWord> {
      TX* tx;
      Read read;

      Reader(TX* tx) : tx(tx), read() {
      }

      void __attribute__((always_inline))
      operator()(void** address, void*& w, uintptr_t mask) const {
          if (uintptr_t missing = mask & ~tx->writes.find(address, w)) {
              uintptr_t mem = (uintptr_t)read(address, tx, missing);
              w = (void*)((uintptr_t)w ^ (((uintptr_t)w ^ mem) & missing));
          }
      }
  };
}

#endif // RSTM_INST_RAW_H
