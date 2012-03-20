CFLAGS += -DLIST_NO_DUPLICATES
CFLAGS += -DMAP_USE_RBTREE

PROG := vacation
BENCHNAMES = client customer manager reservation vacation
LIBNAMES   = list pair mt19937ar random rbtree thread

# simpy typing 'make' dumps a message, rather than trying to guess a default
info:
	@echo "You must specify your platform as the build target."
	@echo "Valid platforms are:"
	@echo "  lib_gcc_solaris_ia32_opt"
	@echo "      library API, gcc, solaris, x86, 32-bit, -O3"
	@echo "  lib_gcc_solaris_ia32_dbg"
	@echo "      library API, gcc, solaris, x86, 32-bit, -O0"

# dispatch to the various platforms.  Make will error unless the platform's
# corresponding definitions are in the build folder
%: ../%.mk 
	MAKEFILES="../$@.mk Vacation.mk Makefile.stm" $(MAKE) default

#
# Simple clean rule: kill all possible folders
#

clean:
	@rm -rf obj.lib_gcc_solaris_ia32_opt obj.lib_gcc_solaris_ia32_dbg

#
# The output directory
#

$(ODIR):
	@mkdir $@
