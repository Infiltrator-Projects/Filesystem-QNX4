# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -ILibraries/include
.PHONY: all check clean
all: build/QNX4.so
build/QNX4.so: native/filesystems/qnx4/qnx4.c native/filesystems/qnx4/format.h native/filesystems/qnx4/check.h native/filesystems/qnx4/journal.h native/filesystems/qnx4/mutation.h native/filesystems/qnx4/payload.h
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-z,defs -o $@ $<
build/test_qnx4: tests/test_qnx4.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_format: tests/test_format.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
build/test_journal: tests/test_journal.c
	mkdir -p build
	$(CC) $(BASE) $(CFLAGS) -o $@ $< -ldl
check: all build/test_qnx4 build/test_format build/test_journal
	test "$$(nm -D --defined-only build/QNX4.so | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	build/test_qnx4 ./build/QNX4.so
	build/test_format ./build/QNX4.so
	build/test_journal ./build/QNX4.so
clean:
	rm -rf build
