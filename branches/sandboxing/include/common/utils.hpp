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
 *  Your basic "useful but don't quite fit anywhere" utilities.
 */

#ifndef COMMON_UTILS_HPP
#define COMMON_UTILS_HPP

#include <cstddef>
#include <cstdlib>
#include <csignal>
#include <cassert>

namespace stm
{
/// We use malloc a couple of times here, and this makes it a bit easier.
template <typename T>
inline T*
typed_malloc(std::size_t numTs)
{
    return static_cast<T*>(malloc(sizeof(T) * numTs));
}

/// We use memcpy in a bunch of places.
template <typename T>
inline T*
typed_memcpy(T* to, const T* from)
{
    return static_cast<T*>(__builtin_memcpy(to, from, sizeof(T)));
}

/// Convince the compiler to tell us how many elements are in a statically
/// sized array. This code appears in a lot of places on the web.
template <typename T, std::size_t NELEMENTS>
inline std::size_t
length_of(T(&)[NELEMENTS])
{
    return NELEMENTS;
}

template <typename T>
inline bool
minimum(T lhs, T rhs)
{
    return (lhs < rhs) ? lhs : rhs;
}

template <typename T>
inline bool
maximum(T lhs, T rhs)
{
    return (lhs > rhs) ? lhs : rhs;
}

/// For protecting against reentrancy.
class Guard {
  public:
    Guard(volatile sig_atomic_t& f) : flag(f) {
        if (flag) {
            assert(false && "Guard failed");
        }
        flag = 1;
    }

    ~Guard() {
        flag = 0;
    }

  private:
    volatile sig_atomic_t& flag;

    Guard();
    Guard(const Guard&);
    Guard& operator=(const Guard&);
};
}


#endif // COMMON_UTILS_HPP
