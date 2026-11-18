# native-runner build.
#
#   make arm           cross build for the Y1: build/runner
#   make bench         interpreter benchmark for the Y1: build/bench, build/guest.bin
#   make arm NDK=...   Android NDK r10e is the default: its GCC and API-17
#                      sysroot work on Android 4.2 without more flags
NDK    ?= $(HOME)/android-ndk-r10e
ARM_CC ?= $(NDK)/toolchains/arm-linux-androideabi-4.9/prebuilt/linux-x86_64/bin/arm-linux-androideabi-gcc
SYSROOT ?= $(NDK)/platforms/android-17/arch-arm
# MAP_FIXED_NOREPLACE is not in the r10e headers; the runner does not rely on it.
ARM_CFLAGS = --sysroot=$(SYSROOT) -std=gnu99 -march=armv7-a -O2 -g -Wall -fPIE -pie -Isrc

SRC     = src/main.c src/machine.c src/cpu.c src/interp.c src/vcpu.c src/privtrap.c src/armemu.c \
          src/dev_soc.c src/dev_gpio.c src/dev_backlight.c src/dev_dma.c src/dev_i2c.c src/dev_i2s.c src/dev_ide.c src/dev_bcm.c src/hostprof.c \
          src/jit.c src/frameout.c src/pv.c src/vdisk.c
HDR     = $(wildcard src/*.h)

.PHONY: all arm bench clean
all: arm

arm: build/runner
build/runner: $(SRC) $(HDR)
	@mkdir -p build
	$(ARM_CC) $(ARM_CFLAGS) $(SRC) -o $@

ARM_BIN = $(dir $(ARM_CC))arm-linux-androideabi-
bench: build/bench build/guest.bin
build/bench: bench/bench.c src/interp.c src/jit.c src/hostprof.c src/vcpu.c src/privtrap.c src/armemu.c $(HDR)
	@mkdir -p build
	$(ARM_CC) $(ARM_CFLAGS) bench/bench.c src/interp.c src/jit.c src/hostprof.c src/vcpu.c src/privtrap.c src/armemu.c -o $@
build/guest.bin: bench/guest.c
	@mkdir -p build
	$(ARM_CC) -std=gnu99 -marm -march=armv5te -O2 -fno-pic -ffreestanding -nostdlib \
	    -Wl,-Ttext=0x10010000 -Wl,-e,bench -o build/guest.elf bench/guest.c
	$(ARM_BIN)objcopy -O binary -j .text build/guest.elf $@

clean:
	rm -rf build
