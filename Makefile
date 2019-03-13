#
# Copyright (c) 2019. Quantum Corporation. All Rights Reserved.
# DXi, StorNext and Quantum are either a trademarks or registered
# trademarks of Quantum Corporation in the US and/or other countries.
#
# FICLONE/FICLONERANGE program makefile.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
# SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
# IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#

CFLAGS := -D_GNU_SOURCE=1 -std=c11

TARGET := cpr
TARGET_SRCS := cpr.c
TARGET_OBJS = $(TARGET_SRCS:.c=.o)

LIBTARGET := libcpr.a
LIBTARGET_SRCS := libcpr.c
LIBTARGET_OBJS = $(LIBTARGET_SRCS:.c=.o)

.phony: all
all: $(LIBTARGET) $(TARGET)

.phony: clean
clean:
	$(RM) $(TARGET_OBJS) $(LIBTARGET_OBJS)
	$(RM) $(TARGET) $(LIBTARGET)

$(TARGET): $(TARGET_OBJS) $(LIBTARGET)
	$(CC) -o $@ $^

$(LIBTARGET): $(LIBTARGET_OBJS)
	$(AR) cr $@ $^
