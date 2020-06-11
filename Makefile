############################################################################
# apps/external/tools/iperf2/Makefile
#
#   Copyright (C) 2018 Pinecone Inc. All rights reserved.
#   Author: zhangyuan7 <zhangyuan7@pinecone.net>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
# 3. Neither the name NuttX nor the names of its contributors may be
#    used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
# FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
# COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
# OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
# AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
# ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
###########################################################################

-include $(TOPDIR)/Make.defs

CXXEXT := .cpp

CSRCS := src/ReportCSV.c
CSRCS += src/ReportDefault.c
CSRCS += src/tcp_window_size.c
CSRCS += src/gnu_getopt.c
CSRCS += src/gnu_getopt_long.c
CSRCS += src/stdio.c
CSRCS += src/sockets.c
CSRCS += src/SocketAddr.c
CSRCS += src/Locale.c
CSRCS += src/service.c
CSRCS += src/Reporter.c
CSRCS += src/Extractor.c
CSRCS += src/checksums.c
CSRCS += src/histogram.c
CSRCS += src/pdfs.c

CSRCS += compat/delay.c
CSRCS += compat/error.c
CSRCS += compat/signal.c
CSRCS += compat/string.c
CSRCS += compat/Thread.c

CXXSRCS := src/Listener.cpp
CXXSRCS += src/Client.cpp
CXXSRCS += src/Server.cpp
CXXSRCS += src/List.cpp
CXXSRCS += src/Launch.cpp
CXXSRCS += src/Settings.cpp
CXXSRCS += src/PerfSocket.cpp
CXXSRCS += src/isochronous.cpp

IPERF2_FLAGS += ${shell $(INCDIR) $(INCDIROPT) "$(CC)" $(SRCDIR)}
IPERF2_FLAGS += ${shell $(INCDIR) $(INCDIROPT) "$(CC)" $(SRCDIR)/include}
IPERF2_FLAGS += ${shell $(TOPDIR)/tools/define.sh "$(CC)" HAVE_CONFIG_H}
IPERF2_FLAGS += -Wno-undef -Wno-shadow

CFLAGS += $(IPERF2_FLAGS) -Wno-strict-prototypes
CXXFLAGS += $(IPERF2_FLAGS)

CONFIG_TOOLS_IPERF2_PRIORITY ?= SCHED_PRIORITY_DEFAULT
CONFIG_TOOLS_IPERF2_STACKSIZE ?= 2048

MAINSRC = src/main.cpp
PROGNAME = iperf2
APPNAME = iperf2
PRIORITY = $(CONFIG_TOOLS_IPERF2_PRIORITY)
STACKSIZE = $(CONFIG_TOOLS_IPERF2_STACKSIZE)

MODULE = CONFIG_TOOLS_IPERF2

include $(APPDIR)/Application.mk
