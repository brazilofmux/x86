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

CORE_SRCS = core/x86_decode.c core/x86_interp.c core/x86_state.c core/x86_mem.c core/x86_paging.c
CORE_OBJS = $(addprefix $(O)/,$(CORE_SRCS:.c=.o))

DBT_SRCS  = dbt/dbt_common.c dbt/dbt_cache.c dbt/dbt_a64.c
DBT_OBJS  = $(addprefix $(O)/,$(DBT_SRCS:.c=.o))
PC_SRCS   = pc/pc_bios.c pc/pc_video.c pc/pc_vga.c pc/pc_sdl.c pc/pc_kbd.c pc/pc_font.c pc/pc_mouse.c pc/pc_disk.c pc/pc_cmos.c
PC_OBJS   = $(addprefix $(O)/,$(PC_SRCS:.c=.o))
DOS_SRCS  = dos/dos_load.c dos/dos_host.c dos/dos_int21.c dos/dos_dpmi.c
DOS_OBJS  = $(addprefix $(O)/,$(DOS_SRCS:.c=.o))

TARGET = $(O)/dos-monster
SST    = $(O)/tools/sst
JITTEST = $(O)/tools/jittest
MAIN_O = $(O)/main.o

.PHONY: test-pm all clean test-sst test-jit test-dos

all: $(TARGET) $(SST) $(JITTEST)

$(TARGET): $(MAIN_O) $(CORE_OBJS) $(DBT_OBJS) $(PC_OBJS) $(DOS_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lz $(SDL_LIBS)

$(O)/pc/pc_sdl.o: pc/pc_sdl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -MMD -MP -c -o $@ $<

$(SST): $(O)/tools/sst.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lz

$(JITTEST): $(O)/tools/jittest.o $(CORE_OBJS) $(DBT_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(O)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(CORE_OBJS:.o=.d) $(DBT_OBJS:.o=.d) $(PC_OBJS:.o=.d) $(DOS_OBJS:.o=.d) $(MAIN_O:.o=.d) $(SST:=.d) $(JITTEST:=.d)

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

test-pm:
	cd tools/pmoracle && nasm -f bin -o pmtest.img pmtest.asm && python3 pmrun.py

# Paging and V86 transcript images: QEMU and dos-monster, diffed
test-pg: $(TARGET)
	cd tools/pmoracle && python3 pgrun.py pgtest.asm && python3 pgrun.py vmtest.asm

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
