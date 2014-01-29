#
#  Copyright (C) 2011
#  University of Rochester Department of Computer Science
#    and
#  Lehigh University Department of Computer Science and Engineering
# 
# License: Modified BSD
#          Please see the file LICENSE.RSTM for licensing information
#

#
# This makefile is for building the RSTM libraries and benchmarks using
# library API, GCC, MacOS, ia32, -O3
#
# Warning: This just handles platform configuration.  Everything else is
#          handled via per-folder Makefiles
#

#
# Compiler config
#
PLATFORM  = gcc_macos_ia32_opt
CXX       = g++
#CXXFLAGS += -Wall -Wextra #-Werror
CXXFLAGS += -O3 -ggdb -m32 -msse2 -mfpmath=sse
LDFLAGS  += -lpthread -m32
CFLAGS   += -m32
ASFLAGS  += -m32
CC        = gcc

#
# Options to pass to STM files
#
CXXFLAGS += -DSTM_API_LIB
CXXFLAGS += -DSTM_CC_GCC
CXXFLAGS += -DSTM_OS_MACOS
CXXFLAGS += -DSTM_CPU_X86
CXXFLAGS += -DSTM_BITS_32
CXXFLAGS += -DSTM_OPT_O3
CXXFLAGS += -DSTM_WS_WORDLOG
CXXFLAGS += -DSTM_PROFILETMTRIGGER_NONE
CXXFLAGS += -DSTM_USE_SSE
CXXFLAGS += -DSTM_CHECKPOINT_SJLJ
CXXFLAGS += -DSTM_NO_PMU
CXXFLAGS += -DSTM_COUNTCONSEC_NO
# Since MacOS tools does not support __thread,
# this is needed for MacOS
CXXFLAGS += -DSTM_TLS_PTHREAD

#TLS
CXXFLAGS += -DSTM_API_TLSPARAM
#ExtraParameter
#CXXFLAGS += -DSTM_API_NOTLSPARAM

#CGA
#CXXFLAGS += -DSTM_INST_COARSEGRAINADAPT
#FGA
CXXFLAGS += -DSTM_INST_FINEGRAINADAPT  
