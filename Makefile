# dos-monster — x86 real/protected-mode DOS DBT

CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS =

CORE_SRCS = core/x86_decode.c core/x86_interp.c core/x86_state.c core/x86_mem.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

DBT_SRCS  = dbt/dbt_common.c dbt/dbt_cache.c dbt/dbt_a64.c
DBT_OBJS  = $(DBT_SRCS:.c=.o)

TARGET = dos-monster

.PHONY: all clean test-sst test-jit

all: tools/sst8088 tools/jittest

tools/sst8088: tools/sst8088.o $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lz

tools/jittest: tools/jittest.o $(CORE_OBJS) $(DBT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(CORE_OBJS:.o=.d) $(DBT_OBJS:.o=.d) tools/sst8088.d tools/jittest.d

# Full 8088 suite (tests/sst8088/*.MOO.gz, not committed — see tests/sst8088/README)
test-sst: tools/sst8088
	./tools/sst8088 -n 3 tests/sst8088/*.MOO.gz

# JIT vs interpreter: built-in programs plus a fuzz sweep, all under -V lockstep
test-jit: tools/jittest
	./tools/jittest -p 0 && ./tools/jittest -p 1 && ./tools/jittest -m 286 -p 0 && \
	./tools/jittest -f 2000 && ./tools/jittest -m 286 -f 2000

clean:
	rm -f $(CORE_OBJS) $(CORE_OBJS:.o=.d) $(DBT_OBJS) $(DBT_OBJS:.o=.d) \
	      tools/sst8088 tools/sst8088.o tools/sst8088.d tools/jittest tools/jittest.o tools/jittest.d $(TARGET)
