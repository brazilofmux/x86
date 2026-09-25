# dos-monster — x86 real/protected-mode DOS DBT

CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS =

# Cross build of the AArch64 backend on an x86-64 Linux host, run through
# qemu-user (binfmt): make ARCH=a64 [test-jit|test-dos]. Objects and the
# binaries go under build-a64/; zlib for arm64 comes from a private
# sysroot (tmp/a64-sysroot: the noble zlib1g and zlib1g-dev debs unpacked).
ifeq ($(ARCH),a64)
  CC = aarch64-linux-gnu-gcc
  O = build-a64
  A64_SYSROOT ?= tmp/a64-sysroot
  CFLAGS += -I$(A64_SYSROOT)/usr/include
  LDFLAGS += -static -L$(A64_SYSROOT)/usr/lib/aarch64-linux-gnu
  NO_SDL = 1
else
  O = .
endif

# The graphics window is optional: without SDL2 the machine is headless.
SDL_CFLAGS := $(if $(NO_SDL),,$(shell pkg-config --cflags sdl2 2>/dev/null))
SDL_LIBS   := $(if $(NO_SDL),,$(shell pkg-config --libs-only-L sdl2 2>/dev/null))
ifneq ($(SDL_CFLAGS),)
  SDL_CFLAGS += -DHAVE_SDL
  SDL_LIBS   += -lSDL2
endif

CORE_SRCS = core/x86_decode.c core/x86_interp.c core/x86_state.c core/x86_mem.c core/x86_paging.c core/x86_fpu.c
# Berkeley SoftFloat 3e (core/softfloat/README): the x87's arithmetic,
# built with upstream's options and its own warnings left alone
SF_SRCS   = $(sort $(wildcard core/softfloat/*.c core/softfloat/8086/*.c))
SF_OBJS   = $(addprefix $(O)/,$(SF_SRCS:.c=.o))
SF_CFLAGS = -O2 -g -std=c11 -w -Icore/softfloat -Icore/softfloat/include -Icore/softfloat/8086 \
            -DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32
CORE_OBJS = $(addprefix $(O)/,$(CORE_SRCS:.c=.o)) $(SF_OBJS)

# The backend: the host's, unless BACKEND=a64|x64 says otherwise (a cross
# check: the other backend's translator runs under X86_GOLDEN, never a block).
HOST_ARCH := $(if $(filter a64,$(ARCH)),aarch64,$(shell uname -m))
BACKEND ?= $(if $(filter aarch64 arm64,$(HOST_ARCH)),a64,x64)
DBT_SRCS  = dbt/dbt_common.c dbt/dbt_cache.c dbt/dbt_translate.c dbt/dbt_$(BACKEND).c
DBT_OBJS  = $(addprefix $(O)/,$(DBT_SRCS:.c=.o))
PC_SRCS   = pc/pc_bios.c pc/pc_video.c pc/pc_vga.c pc/pc_sdl.c pc/pc_kbd.c pc/pc_font.c pc/pc_mouse.c pc/pc_disk.c pc/pc_cmos.c pc/pc_ps2.c pc/pc_ide.c
PC_OBJS   = $(addprefix $(O)/,$(PC_SRCS:.c=.o))
DOS_SRCS  = dos/dos_load.c dos/dos_host.c dos/dos_int21.c dos/dos_dpmi.c
DOS_OBJS  = $(addprefix $(O)/,$(DOS_SRCS:.c=.o))

TARGET = $(O)/dos-monster
SST    = $(O)/tools/sst
JITTEST = $(O)/tools/jittest
MAIN_O = $(O)/main.o

.PHONY: test-pm all clean test-sst test-jit test-dos test-x64enc test-hwflags test-fpu

all: $(TARGET) $(SST) $(JITTEST)

$(TARGET): $(MAIN_O) $(CORE_OBJS) $(DBT_OBJS) $(PC_OBJS) $(DOS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lz $(SDL_LIBS)

# The fixed-disk BIOS in real instructions (tools/diskbios.asm), assembled
# into a header that is checked in: a build needs NASM only after an edit
pc/pc_diskbios.h: tools/diskbios.asm
	@mkdir -p $(O)/tools
	nasm -f bin -o $(O)/tools/diskbios.bin $< && \
	python3 -c "import sys; b=open(sys.argv[1],'rb').read(); print('/* tools/diskbios.asm, assembled (make pc/pc_diskbios.h) */'); print('static const unsigned char diskbios[%d] = {' % len(b)); [print('    ' + ', '.join('0x%02X' % x for x in b[i:i+16]) + ',') for i in range(0, len(b), 16)]; print('};')" $(O)/tools/diskbios.bin > $@
$(O)/pc/pc_disk.o: pc/pc_diskbios.h

$(O)/pc/pc_sdl.o: pc/pc_sdl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -MMD -MP -c -o $@ $<

$(SST): $(O)/tools/sst.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lz

$(JITTEST): $(O)/tools/jittest.o $(CORE_OBJS) $(DBT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(O)/core/softfloat/%.o: core/softfloat/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SF_CFLAGS) -MMD -MP -c -o $@ $<

# the x87 uses SoftFloat's internals (its round-and-pack), so it is built
# with SoftFloat's configuration too
$(O)/core/x86_fpu.o: core/x86_fpu.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -isystem core/softfloat/include -isystem core/softfloat/8086 -Icore \
	    -DSOFTFLOAT_FAST_INT64 -DINLINE_LEVEL=5 -MMD -MP -c -o $@ $<

$(O)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(CORE_OBJS:.o=.d) $(DBT_OBJS:.o=.d) $(PC_OBJS:.o=.d) $(DOS_OBJS:.o=.d) $(MAIN_O:.o=.d) $(SST:=.d) $(JITTEST:=.d) $(HWFLAGS:=.d)

# SingleStepTests suites (tests/sst*/, not committed — see tests/sst8088/fetch.sh, tests/sst386/fetch.sh)
test-sst: $(SST)
	$(SST) -n 3 tests/sst8088/*.MOO.gz
test-sst286: $(SST)
	$(SST) -n 3 tests/sst286/*.MOO.gz
test-sst386: $(SST)
	$(SST) -n 3 tests/sst386/*.MOO.gz

# JIT vs interpreter: built-in programs plus a fuzz sweep, all under -V lockstep
test-jit: $(JITTEST)
	$(JITTEST) -p 0 && $(JITTEST) -p 1 && $(JITTEST) -m 286 -p 0 && \
	$(JITTEST) -f 2000 && $(JITTEST) -m 286 -f 2000 && $(JITTEST) -m 386 -f 2000 && \
	$(JITTEST) -P -f 3000 && $(JITTEST) -P -f 1000 -r 100000 -n 40 && \
	$(JITTEST) -G -f 3000 && $(JITTEST) -G -f 1000 -r 100000 -n 40

# DOS-level smoke tests (tests/dos/run.sh; uses disks/tp55 when present)
test-dos: $(TARGET)
	DM=$(TARGET) tests/dos/run.sh

test-boot: $(TARGET)
	DM=$(TARGET) tests/boot/run.sh

# dbt/emit_x64.h against objdump (tools/x64enc.c: one line per encoding)
test-x64enc:
	tools/x64enc.sh

# The host's flags against the interpreter's for the ops Intel leaves
# undefined: what the x86-64 backend has to fix up (x86-64 hosts only)
HWFLAGS = $(O)/tools/hwflags
$(HWFLAGS): $(O)/tools/hwflags.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
test-hwflags: $(HWFLAGS)
	$(HWFLAGS) -m 386 -n 2000

test-pm:
	cd tools/pmoracle && nasm -f bin -o pmtest.img pmtest.asm && python3 pmrun.py

# Paging and V86 transcript images: QEMU and dos-monster, diffed
test-pg: $(TARGET)
	cd tools/pmoracle && python3 pgrun.py pgtest.asm && python3 pgrun.py vmtest.asm && \
	    python3 pgrun.py c486test.asm --m486 --expect c486test.expected && \
	    python3 pgrun.py c486test.asm --m486 --expect c486test.expected -- -V && \
	    python3 pgrun.py c586test.asm --m586 --expect c586test.expected && \
	    python3 pgrun.py c586test.asm --m586 --expect c586test.expected -- -V

# The x87 against its transcript (tools/pmoracle/fpurun.py): the interpreter,
# the JIT and -V
test-fpu: $(TARGET)
	cd tools/pmoracle && python3 fpurun.py --expect fputest.expected -- -i && \
	    python3 fpurun.py --expect fputest.expected && python3 fpurun.py --expect fputest.expected -- -V

test-pm-compare:
	cd tools/pmoracle && nasm -f bin -o pmtest.img pmtest.asm && python3 compare.py

# The oracle image again, through protected-mode JIT blocks under -V lockstep
test-pm-jit: $(TARGET)
	cd tools/pmoracle && nasm -f bin -o pmtest.img pmtest.asm && \
	! ../../dos-monster -boot pmtest.img -V -m 386 -L 5000000 2>&1 >/dev/null | grep -A12 divergence && \
	echo "pm oracle image: -V clean"

clean:
	rm -f $(CORE_OBJS) $(CORE_OBJS:.o=.d) $(DBT_OBJS) $(DBT_OBJS:.o=.d) \
	      $(PC_OBJS) $(PC_OBJS:.o=.d) $(DOS_OBJS) $(DOS_OBJS:.o=.d) main.o main.d \
	      tools/sst tools/sst.o tools/sst.d tools/jittest tools/jittest.o tools/jittest.d dos-monster
	rm -rf build-a64
