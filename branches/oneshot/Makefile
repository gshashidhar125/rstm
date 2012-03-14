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
# to achieve this end without significant makefile wizardry
#

# simpy typing 'make' dumps a message, rather than trying to guess a default
info:
	@echo "You must specify your platform as the build target."
	@echo "Valid platforms are:"
	@echo "  lib_gcc_solaris_ia32_opt"
	@echo "      library API, gcc, solaris, x86, 32-bit, -O3"



# dispatch to the various platforms
lib_gcc_solaris_ia32_opt:
	MAKEFILES=build/$@.mk $(MAKE) all

#lib_suncc_solaris_x86_opt:
