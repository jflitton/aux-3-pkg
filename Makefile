# pkg and its tests, built two ways: for A/UX 3 with m68k-elf-gcc and our runtime (rt/),
# and for this machine with its own compiler and libc, for fast tests. See
# docs/development.md. Written for macOS's make 3.81.
# (No AddressSanitizer on the host build: on macOS 26 it hangs before main.)

AUX_CC = m68k-elf-gcc
HOST_CC = cc
WARN = -Wall -Wextra -Wno-unused-parameter
AUX_CFLAGS = -m68020 -msoft-float -Os -ffreestanding -fno-asynchronous-unwind-tables \
	$(WARN) -DAUX -Irt -Isrc -Ibuild -MMD -MP
AUX_LDFLAGS = -nostdlib -static -T rt/aux.ld -Wl,--build-id=none \
	-Wl,--no-warn-rwx-segments
HOST_CFLAGS = -O1 -g -fsanitize=undefined -fno-sanitize-recover=all $(WARN) -Isrc \
	-Ibuild -MMD -MP '-DDEFAULT_SERVER=""'
PYTHON = python3 -I

# For tests in QEMU: a stock A/UX 3.1 disk image (never changed: tests boot a clone), a
# Quadra 800 ROM, and aux-3-patches, whose tools read and write A/UX disks. Set AUX_DISK
# and AUX_ROM in local.mk (ignored by git) or on make's command line.
-include local.mk
AUX_PATCHES ?= ../aux-3-patches
export AUX_DISK AUX_ROM AUX_PATCHES

RT = build/aux/rt/crt0.o build/aux/rt/sys.o build/aux/rt/libc.o
LIB = sha ed25519 gunzip util tar db install http repo
AUX_LIB = $(LIB:%=build/aux/src/%.o)
HOST_LIB = $(LIB:%=build/host/src/%.o)
AUX_PROGS = build/aux/rt-test build/aux/unit build/aux/httpd build/aux/pkg

all: $(AUX_PROGS) build/host/unit build/host/pkg

build/aux/%.o: %.c
	@mkdir -p $(@D)
	$(AUX_CC) $(AUX_CFLAGS) -c -o $@ $<

build/aux/%.o: %.S
	@mkdir -p $(@D)
	$(AUX_CC) $(AUX_CFLAGS) -c -o $@ $<

build/host/%.o: %.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -c -o $@ $<

build/aux/rt-test.elf: build/aux/test/rt-test.o $(RT) rt/aux.ld
build/aux/unit.elf: build/aux/test/unit.o $(AUX_LIB) $(RT) rt/aux.ld
build/aux/httpd.elf: build/aux/test/httpd.o $(RT) rt/aux.ld
build/aux/pkg.elf: build/aux/src/main.o $(AUX_LIB) $(RT) rt/aux.ld
$(AUX_PROGS:%=%.elf):
	$(AUX_CC) $(AUX_CFLAGS) $(AUX_LDFLAGS) -o $@ $(filter %.o,$^) -lgcc

$(AUX_PROGS): build/aux/%: build/aux/%.elf tools/elf2aout.py
	$(PYTHON) tools/elf2aout.py $< $@

build/host/unit: build/host/test/unit.o $(HOST_LIB)
build/host/pkg: build/host/src/main.o $(HOST_LIB)
build/host/unit build/host/pkg:
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^

build/aux/test/unit.o build/host/test/unit.o: build/unit-vectors.h
build/unit-vectors.h: test/gen-unit.py tools/ed25519.py
	@mkdir -p $(@D)
	$(PYTHON) test/gen-unit.py $@

# On this machine
test: build/host/unit build/host/pkg
	build/host/unit
	$(PYTHON) test/host-test.py build/host/pkg

build/aux-repo: test/make-repo.py tools/pkg-build.py tools/pkg-repo.py
	rm -rf $@
	$(PYTHON) test/make-repo.py $@

# In A/UX, booted in QEMU
test-aux: $(AUX_PROGS) build/aux-repo
	$(PYTHON) tools/aux-run.py test/aux-test.sh $(AUX_PROGS) build/aux-repo/*

# pkg as a package for its server, build/packages/pkg-VERSION.pkg, with the record
# aux-3-patches' --pkg gives it (docs/development.md, "Releasing pkg")
package: build/aux/pkg tools/pkg-build.py
	rm -rf build/package
	mkdir -p build/package/usr/local/bin build/packages
	cp build/aux/pkg build/package/usr/local/bin/pkg
	chmod 755 build/package/usr/local/bin/pkg
	{ echo 'format 1'; echo 'name pkg'; \
	  sed -n 's/.*PKG_VERSION[^"]*"\(.*\)".*/version \1/p' src/pkg.h; \
	  echo 'summary Installs, upgrades and removes packages'; \
	  echo 'url https://github.com/jflitton/aux-3-pkg'; \
	  git describe --tags --always --dirty 2>/dev/null | sed 's/^/built /'; \
	} > build/package/+PKG
	$(PYTHON) tools/pkg-build.py build/package build/packages

clean:
	rm -rf build

.PHONY: all test test-aux package clean
.SECONDARY:

-include $(shell find build -name '*.d' 2>/dev/null)
