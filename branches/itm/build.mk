#
#  Copyright (C) 2011
#  University of Rochester Department of Computer Science
#    and
#  Lehigh University Department of Computer Science and Engineering
# 
# License: Modified BSD
#          Please see the file LICENSE.RSTM for licensing information

#
# The top-level RSTM makefile simply forwards to a makefile that is specific
# to a particular build platform.  We use the MAKEFILES environment variable
# to achieve this end without significant makefile wizardry.
#

#
# Location of helper build files
#
MKFOLDER = build
include $(MKFOLDER)/info.mk

#
# Allow V=1 on the command line to display verbose make lines.
#
ifndef V
export _V=@
endif

#
# Names of all folders we might want to clean out
#
OUTDIRS	     = $(patsubst %, obj.%, $(PLATFORMS))
LIBOUTDIRS   = $(patsubst %, lib/%, $(OUTDIRS))
BENCHOUTDIRS = $(patsubst %, bench/%, $(OUTDIRS))

#
# simply typing 'make' dumps a message, rather than trying to guess a default
# platform
#
default: info

#
# Perform a build in the lib/ folder, and then a build in the bench/ folder,
# using the MAKEFILES envar to specify the platform.
#
# $(MAKE) will error unless the platform's corresponding definitions are in
# $(MKFOLDER)
#
%: $(MKFOLDER)/%.mk
	@cd lib && $(MAKE) $@
	@cd bench && $(MAKE) $@

#
# Simple clean rule: kill all possible folders
#
clean:
	$(_V)$(RM)r $(LIBOUTDIRS) $(BENCHOUTDIRS)
