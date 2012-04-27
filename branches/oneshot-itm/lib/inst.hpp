#ifndef RSTM_INST_H
#define RSTM_INST_H

#include "byte-logging.hpp"             // Word, MaskedWord, LogWordType
#include "inst-readonly.hpp"            // CheckWritesetForReadOnly
#include "inst-stackfilter.hpp"         // NoFilter
#include "inst-writer.hpp"              // oWriter<>
#include "inst-reader.hpp"              // Reader<>
#include "inst-alignment.hpp"           // Aligned<,>
#include "inst-common.hpp"              // make_mask, min

// This is all inline stuff that shouldn't ever be visible from outside of the
// source file in which it's included, so we stick it all in an anonymouse
// namespace.
namespace {
  using namespace stm;

  /**
   *  This template and its specializations select between two types.
   *
   *  if (F1 != NullType)
   *      Result = F1
   *  else if (F2 != NullType)
   *      Result = F2
   *  else
   *      Error
   */
  template <typename F1, typename F2>
  struct SelectNonNull {
      typedef F1 Result;
  };

  template <typename F2>
  struct SelectNonNull<NullType, F2> {
      typedef F2 Result;
  };

  template <>
  struct SelectNonNull<NullType, NullType>;

  template <typename T,
            bool ForceAligned,
            typename WordType,
            typename IsReadOnly,
            typename ReadFilter,
            typename ReadRW,
            typename ReadReadOnly,
            typename WriteFilter,
            typename WriteRW,
            typename WriteReadOnly>
  class GenericInst {
    private:
      /**
       *  The number of words we need to reserve to deal with a T, is basically
       *  the number of bytes in a T divided by the number of bytes in a void*,
       *  plus one if a T* might not be aligned. The caveat is that we need at
       *  least one word for aligned subword types. The math below is evaluated
       *  at compile time.
       */
      enum {
          N = ((sizeof(T)/sizeof(void*)) ? sizeof(T)/sizeof(void*) : 1) +
              ((Aligned<T, ForceAligned>::value) ? 0 : 1)
      };

      /**
       *  Pick the correct read-only instrumentation (just in case the client
       *  uses NullType to indicate that there isn't any read-only-specific
       *  option).
       */
      typedef typename SelectNonNull<ReadReadOnly, ReadRW>::Result ReadRO;
      typedef typename SelectNonNull<WriteReadOnly, WriteRW>::Result WriteRO;

      /**
       *  Used by ITM to log values into the undo log. Supports the _ITM_LOG
       *  interface.
       */
      struct Logger {
          void operator()(void** addr, void* val, TX* tx, uintptr_t mask) const {
              tx->undo_log.insert(addr, val, mask);
          }
      };

      static inline size_t OffsetOf(const T* const addr) {
          return (Aligned<T, ForceAligned>::value) ? 0 : offset_of(addr);
      }

      static inline void** BaseOf(T* const addr) {
          return (Aligned<T, ForceAligned>::value) ? (void**)addr : base_of(addr);
      }

      /**
       *  This is the fundamental loop used for both chunked read and write
       *  access. It's basic job is to loop through each word in the words[]
       *  array, and perform f() on it. This should be customized and inlined
       *  for each type of F, and N (and possibly OffsetOf and BaseOf) is a
       *  compile-time constant, so this should be optimized nicely.
       */
      template <typename F, size_t N>
      static void __attribute__((always_inline))
      ProcessWords(T* addr, void* (&words)[N], const F& f) {
          // get the base and the offset of the address, in case we're dealing
          // with a sub-word or unaligned access. BaseOf and OffsetOf return
          // constants whenever they can.
          void** const base = BaseOf(addr);
          const size_t off = OffsetOf(addr);

          // deal with the first word (there's always at least one)
          uintptr_t mask = make_mask(off, min(sizeof(void*), off + sizeof(T)));
          f(base, words[0], mask);

          // deal with any middle words for large types
          mask = make_mask(0, sizeof(void*));
          for (size_t i = 1, e = N - 1; i < e; ++i)
              f(base + i, words[i], mask);

          // if we have a final word to read, do so
          if (N > 1 && (off + sizeof(T) > sizeof(void*))) {
              mask = make_mask(0, off);
              f(base + N - 1, words[N - 1], mask);
          }
      }

    public:
      /**
       *  The client's read instrumentation, generally inlined into the
       *  externally visible read routine (alg_tm_read for the library API,
       *  _ITM_R<T> for the ITM API).
       */
      static T Read(T* addr) {
          TX* tx = Self;

          // Use the configured pre-filter to do an "in-place" access if we
          // need to.
          ReadFilter filter;
          if (filter(addr, tx))
              return *addr;

          // Allocate space on the stack to perform the chunks of the
          // operation. The union is necessary for dealing with subword or
          // unaligned accesses. This all gets optimized because N is a
          // compile-time constant.
          union {
              void* words[N];
              uint8_t bytes[sizeof(void*[N])];
          };

          // If this transaction is read-only, then we don't need to do RAW
          // checks and we should use the ReadRO function that we're
          // configured with. Otherwise, do RAW checks based on the configured
          // WordType.
          IsReadOnly readonly;
          if (readonly(tx))
              ProcessWords(addr, words, Reader<ReadRO, NullType>(tx));
          else
              ProcessWords(addr, words, Reader<ReadRW, WordType>(tx));

          // use the 'bytes' half of the union to return the value as the
          // correct type
          return *reinterpret_cast<T*>(bytes + OffsetOf(addr));
      }

      /**
       *  The client's write instrumentation, generally inlined into the
       *  externally visible write routine.
       */
      static void Write(T* addr, T val) {
          TX* tx = Self;

          // Use the configured pre-filter to do an "in-place" access if we
          // need to.
          WriteFilter filter;
          if (filter(addr, tx)) {
              *addr = val;
              return;
          }

          // Allocate space on the stack to perform the chunks of the
          // operation. The union is necessary for dealing with subword or
          // unaligned accesses. This all gets optimized because N is a
          // compile-time constant.
          union {
              void* words[N];
              uint8_t bytes[sizeof(void*[N])];
          };

          // Put the to-write value into the union at the right offset.
          *reinterpret_cast<T*>(bytes + OffsetOf(addr)) = val;

          // If this transaction is readonly, then we use the configured
          // WriteRO functor, otherwise we use the Write functor.
          IsReadOnly readonly;
          if (readonly(tx))
              ProcessWords(addr, words, Writer<WriteRO>(tx));
          else
              ProcessWords(addr, words, Writer<WriteRW>(tx));
      }

      /**
       *  The client's log instrumentation, generally inlined into the
       *  externally visible log routines.
       */
      static void Log(T* addr) {
          TX* tx = Self;

          // We don't filter stack logs---presumably there is a reason that the
          // compiler has generated a log of the transactional stack. This will
          // cause issues for rollback loops, if the address corrupts the stack
          // in a way that impacts the pre-longjmp execution.

          // Allocate space on the stack to perform the log.
          union {
              void* words[N];
              uint8_t bytes[sizeof(void*[N])];
          };

          // Put the to-log value into the union at the right offset.
          *reinterpret_cast<T*>(bytes + OffsetOf(addr)) = *addr;

          // repurpose the undo log for logging.
          ProcessWords(addr, words, Writer<Logger>(tx));
      }
  };

  /**
   *  Many of our Lazy TMs use the same instrumentation configuration, other
   *  than the read algorithm. This "Lazy" adapter fixes the type parameters of
   *  the GenericInst that they don't care about.
   *
   *  Barriers can be instantiated like Lazy<uint8_t, ReadAlg>::RSTM::Read().
   */
  template <typename T, typename Read>
  struct Lazy {
      struct BufferedWrite {
          void operator()(void** addr, void* val, TX* tx, uintptr_t mask) const {
              tx->writes.insert(addr, val, mask);
          }
      };

      typedef GenericInst<T, true, Word, CheckWritesetForReadOnly,
                          NoFilter, Read, NullType,
                          NoFilter, BufferedWrite, NullType> RSTM;

      typedef GenericInst<T, false, LoggingWordType, CheckWritesetForReadOnly,
                          FullFilter, Read, NullType,
                          FullFilter, BufferedWrite, NullType> ITM;
  };
}

#endif // RSTM_INST_H
