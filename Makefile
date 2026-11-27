# SPDX-License-Identifier: GPL-3.0-or-later
CC ?= cc
CFLAGS ?= -O2
BASE = -std=c11 -Wall -Wextra -Werror -ILibraries/include
.PHONY: all check clean
all: build/QNX4.so
build/QNX4.so: native/filesystems/qnx4/qnx4.c native/filesystems/qnx4/format.h native/filesystems/qnx4/check.h native/filesystems/qnx4/journal.h native/filesystems/qnx4/mutation.h native/filesystems/qnx4/payload.h native/filesystems/qnx4/relocation.h
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

# The installed carrier has a distinct baseline firmware ELF with no libc.
OBJCOPY ?= objcopy
FIRMWARE_FLAGS = -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -mno-red-zone -mgeneral-regs-only -fcf-protection=none -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fPIC -fvisibility=hidden
FIRMWARE_LINK = -shared -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now,-z,nopack-relative-relocs,-z,max-page-size=4096
.PHONY: firmware carrier
firmware: build/firmware/QNX4.so
build/firmware/QNX4.so: native/filesystems/qnx4/qnx4.c module/firmware/runtime.c $(wildcard native/filesystems/qnx4/*.h) Makefile
	mkdir -p build/firmware
	$(CC) $(FIRMWARE_FLAGS) -DIFS_BOOT_FIRMWARE=1 -ILibraries/include $(FIRMWARE_LINK) -o $@ native/filesystems/qnx4/qnx4.c module/firmware/runtime.c
	test "$$(nm -D --defined-only $@ | awk '{print $$3}')" = infiltrator_filesystem_api_v2
	test -z "$$(nm -D --undefined-only $@)"
	! readelf -dW $@ | grep -E 'NEEDED|SONAME|INIT_ARRAY|FINI_ARRAY|RELR'
carrier: build/QNX4.so firmware
	mkdir -p build/carrier
	$(OBJCOPY) --add-section .infiltrator_boot=build/firmware/QNX4.so --set-section-flags .infiltrator_boot=readonly build/QNX4.so build/carrier/QNX4.so
	$(OBJCOPY) --dump-section .infiltrator_boot=build/carrier/verify.elf build/carrier/QNX4.so build/carrier/verify-carrier.elf
	cmp build/firmware/QNX4.so build/carrier/verify.elf
	rm -f build/carrier/verify.elf build/carrier/verify-carrier.elf
