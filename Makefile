# dos-monster — x86 real/protected-mode DOS DBT

CC      = cc
CFLAGS  = -Wall -Wextra -O2 -g -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS =

CORE_SRCS = core/x86_decode.c core/x86_interp.c core/x86_state.c
CORE_OBJS = $(CORE_SRCS:.c=.o)

TARGET = dos-monster

.PHONY: all clean test-sst

all: tools/sst8088

tools/sst8088: tools/sst8088.o $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lz

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(CORE_OBJS:.o=.d) tools/sst8088.d

# Full 8088 suite (tests/sst8088/*.MOO.gz, not committed — see tests/sst8088/README)
test-sst: tools/sst8088
	./tools/sst8088 -n 3 tests/sst8088/*.MOO.gz

clean:
	rm -f $(CORE_OBJS) $(CORE_OBJS:.o=.d) tools/sst8088 tools/sst8088.o tools/sst8088.d $(TARGET)
