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
 * This API file simply forwards to an algorithm-specific mode of
 * instrumentation.  This lets us build a benchmark using the correct form of
 * instrumentation (e.g., possibly inlined).
 */

#ifndef STM_H__
#define STM_H__

/**
 *  We currently support three modes of instrumentation: CGL, which is for
 *  when we do not need to instrument reads and writes at all, TML, for when
 *  we inline read and write instrumentation via the TML algorithm, and STM,
 *  which uses standard function call instrumentation for reads and writes.
 */

#if defined(STM_INST_CGL)
#  include "cglapi.hpp"
#elif defined(STM_INST_TML)
#  include "tmlapi.hpp"
#elif defined(STM_INST_GENERIC)
#  include "stmapi.hpp"
#else
#  error "Unrecognized STM instrumentation mode"
#endif

#endif // STM_H__
