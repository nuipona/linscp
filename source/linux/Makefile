# Native POSIX (Debian/Linux) build of LinuxSCP.
#
# This builds the console client `linuxscp` for Linux out of:
#
#   * the platform-neutral PuTTY core that the Windows build also uses
#     (source/putty, see putty-core.mk),
#   * a POSIX platform layer vendored from upstream PuTTY 0.85
#     (libs/putty/unix, libs/putty/charset),
#   * the upstream PuTTY SFTP client library (libs/putty/ssh/sftp.c),
#   * the LinuxSCP front end in this directory.
#
# Usage:
#   make -C source/linux            # builds build/linuxscp
#   make -C source/linux check      # runs the unit tests
#   make -C source/linux clean
#
# See ../../docs/debian.md for the Debian package build.

TOP := ../..
BUILD ?= build

# No built-in rules: we build every object explicitly below.
MAKEFLAGS += -r
OBJDIR := $(BUILD)/obj
BINDIR := $(BUILD)

CC ?= cc
CXX ?= c++
AR ?= ar

OPT ?= -O2
WARN ?= -Wall

# -DFORK_BUILD               : build the PuTTY core in "embedded library" mode,
#                              as the Windows build does.
# -DPLATFORM_HAS_SMEMCLR     : the core provides smemclr().
# -DNO_GSSAPI                : no GSSAPI/Kerberos support in this build.
# -DHAVE_...                 : C library features available on Linux.
# -DHAVE_AES_NI=0 etc.       : use the portable crypto implementations; the
#                              x86 intrinsics versions in source/putty/crypto
#                              are tuned for the MSVC/BCC Windows toolchains.
PUTTY_DEFS := \
	-DFORK_BUILD \
	-DMPEXT \
	-DPLATFORM_HAS_SMEMCLR \
	-DNO_GSSAPI \
	-DHAVE_AES_NI=0 \
	-DHAVE_SHA_NI=0 \
	-DHAVE_CLMUL=0 \
	-DHAVE_NEON_CRYPTO=0 \
	-DHAVE_NEON_PMULL=0 \
	-DHAVE_NEON_SHA512=0 \
	-DHAVE_SYS_TYPES_H=1 \
	-DHAVE_SYS_STAT_H=1 \
	-DHAVE_SYS_TIME_H=1 \
	-DHAVE_SYS_SELECT_H=1 \
	-DHAVE_SYS_SOCKET_H=1 \
	-DHAVE_SYS_IOCTL_H=1 \
	-DHAVE_SYS_UN_H=1 \
	-DHAVE_SYS_UIO_H=1 \
	-DHAVE_NETDB_H=1 \
	-DHAVE_NETINET_IN_H=1 \
	-DHAVE_ARPA_INET_H=1 \
	-DHAVE_POLL_H=1 \
	-DHAVE_FCNTL_H=1 \
	-DHAVE_TERMIOS_H=1 \
	-DHAVE_UNISTD_H=1 \
	-DHAVE_PWD_H=1 \
	-DHAVE_GLOB_H=1 \
	-DHAVE_POLL=1 \
	-DHAVE_CLOCK_GETTIME=1 \
	-DHAVE_CLOCK_MONOTONIC=1 \
	-DHAVE_SETPWENT=1 \
	-DHAVE_ENDPWENT=1 \
	-DHAVE_FUTIMES=1 \
	-DHAVE_GETADDRINFO=1 \
	-DHAVE_UTIMES=1 \
	-DHAVE_SYSCTLBYNAME=0 \
	-DHAVE_SO_PEERCRED=1 \
	-DHAVE_GETAUXVAL=1 \
	-DHAVE_SYS_AUXV_H=1 \
	-DHAVE_ASM_HWCAP_H=1 \
	-DHAVE_ELF_AUX_INFO=0 \
	-DHAVE_SYS_SYSCTL_H=0

PUTTY_INCLUDES := \
	-I$(TOP)/source/putty \
	-I$(TOP)/libs/putty/unix \
	-I$(TOP)/libs/putty \
	-I$(TOP)/libs/putty/charset

CFLAGS_PUTTY := -std=gnu99 $(OPT) -g $(WARN) -Wno-unused-parameter \
	$(PUTTY_DEFS) $(PUTTY_INCLUDES) $(CFLAGS)

CXXFLAGS_APP := -std=gnu++17 $(OPT) -g $(WARN) \
	-I. -I../core -I$(TOP)/source/putty -I$(TOP)/libs/putty/unix \
	-I$(TOP)/libs/putty $(CXXFLAGS)

include putty-core.mk

# POSIX platform layer, vendored from upstream PuTTY 0.85.
PUTTY_VEND_SRCS := \
	$(wildcard $(TOP)/libs/putty/unix/*.c) \
	$(wildcard $(TOP)/libs/putty/unix/utils/*.c) \
	$(wildcard $(TOP)/libs/putty/unix/stubs/*.c) \
	$(filter-out $(TOP)/libs/putty/charset/enum.c,$(wildcard $(TOP)/libs/putty/charset/*.c)) \
	$(TOP)/libs/putty/ssh/sftp.c \
	$(TOP)/libs/putty/psftpcommon.c

PUTTY_ALL_SRCS := $(PUTTY_CORE_SRCS) $(PUTTY_VEND_SRCS)

# Object file names mirror the source tree, rooted at the repository top.
putty_obj = $(OBJDIR)/$(patsubst $(TOP)/%.c,%.o,$(1))

PUTTY_OBJS := $(foreach s,$(PUTTY_ALL_SRCS),$(call putty_obj,$(s)))

# Front end (this directory) and the shared console code.
APP_SRCS := \
	Session.cpp \
	SftpClient.cpp \
	PuttyGlue.cpp \
	Script.cpp \
	Console.cpp \
	LocalFs.cpp \
	PathUtil.cpp \
	Texts.cpp \
	main.cpp

APP_OBJS := $(addprefix $(OBJDIR)/linux/,$(APP_SRCS:.cpp=.o))
TEST_OBJS := $(OBJDIR)/linux/tests/UnitTest.o

PUTTY_LIB := $(BUILD)/libputty.a

.PHONY: all clean check

all: $(BINDIR)/linuxscp

$(PUTTY_LIB): $(PUTTY_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BINDIR)/linuxscp: $(APP_OBJS) $(PUTTY_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_APP) -o $@ $(APP_OBJS) $(PUTTY_LIB) $(LDFLAGS) $(LIBS)

$(OBJDIR)/source/%.o: $(TOP)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_PUTTY) -c $< -o $@

$(OBJDIR)/libs/%.o: $(TOP)/libs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_PUTTY) -c $< -o $@

$(OBJDIR)/linux/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_APP) -c $< -o $@

$(OBJDIR)/linux/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_APP) -Itests -c $< -o $@

$(BINDIR)/unittest: $(TEST_OBJS) $(PUTTY_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_APP) -Itests -o $@ $(TEST_OBJS) $(PUTTY_LIB) $(LDFLAGS) $(LIBS)

check: $(BINDIR)/unittest
	$(BINDIR)/unittest

clean:
	rm -rf $(BUILD)
