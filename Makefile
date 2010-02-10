# Versioning
VERSION = 0.1
NAME = Klaatu

# Tools
CC = gcc
CPP = cpp
CFLAGS = -O2 -pipe
LDFLAGS =

# Paths
DESTDIR =
prefix = /usr
bindir = $(prefix)/bin
libdir = $(prefix)/lib

# Compilation parameters
SCFLAGS = -Wall -Wextra -Wno-unused-parameter -fPIC $(CFLAGS)
SLDFLAGS = -ldl $(LDFLAGS)

# Debug
ifeq ($(D),1)
SCFLAGS += -O0 -g
endif

# Build parameters
ifeq ($(V),1)
Q =
cmd = $(2)
else
Q = @
cmd = echo $(1); $(2)
endif

# User targets
all: libsheep sheep

libsheep: sheep/libsheep-$(VERSION).so

sheep: sheep/sheep

# Build targets
include sheep/Makefile
libsheep-obj := $(addprefix sheep/, $(libsheep-obj))
sheep-obj := $(addprefix sheep/, $(sheep-obj))

sheep/libsheep-$(VERSION).so: $(libsheep-obj)
	$(Q)$(call cmd, "   LD     $@",					\
		$(CC) $(SCFLAGS) -o $@ $^ $(SLDFLAGS) -shared)

sheep/sheep: libsheep $(sheep-obj)
	$(Q)$(call cmd, "   LD     $@",					\
		$(CC) $(SCFLAGS) -Lsheep -o $@ $(sheep-obj) -lsheep-$(VERSION))

$(libsheep-obj): include/sheep/config.h sheep/make.deps

include/sheep/config.h: Makefile
	$(Q)$(call cmd, "   CF     $@",					\
		rm -f $@;						\
		echo "#define SHEEP_VERSION \"$(VERSION)\"" >> $@;	\
		echo "#define SHEEP_NAME \"$(NAME)\"" >> $@)

sheep/make.deps:
	$(Q)$(call cmd, "   MK     $@",					\
		rm -f $@;						\
		$(foreach obj,$(libsheep-obj),				\
			$(CPP) -Iinclude -MM -MT $(obj)			\
				$(basename $(obj)).c >> $@; ))

# Cleanup
ifneq ($(MAKECMDGOALS),clean)
-include sheep/make.deps
endif

clean := sheep/libsheep-$(VERSION).so $(libsheep-obj)
clean += sheep/sheep $(sheep-obj)
clean += include/sheep/config.h sheep/make.deps

clean:
	$(Q)$(foreach subdir,$(sort $(dir $(clean))),			\
		$(call cmd, "   CL     $(subdir)",			\
			rm -f $(filter $(subdir)%,$(clean)); ))

# Build library
%.o: %.c
	$(Q)$(call cmd, "   CC     $@",					\
		$(CC) $(SCFLAGS) -Iinclude -o $@ -c $<)

# Misc
.PHONY: all libsheep sheep clean
