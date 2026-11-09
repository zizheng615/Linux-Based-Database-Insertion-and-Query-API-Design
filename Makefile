CC      = gcc
AR      = ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc -pthread
LDFLAGS += -pthread

LIB  = libtablelite.a
OBJS = src/table.o src/index.o src/btree.o src/util.o
EXE  =

# Windows 仅用于本地调试：额外编译 pread/pwrite 兼容层
ifeq ($(OS),Windows_NT)
OBJS += src/plat_win32.o
EXE   = .exe
endif

all: $(LIB) tests/test_table$(EXE) tests/bench$(EXE) examples/demo$(EXE)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

src/%.o: src/%.c include/table.h src/btree.h src/index.h src/table_internal.h src/util.h
	$(CC) $(CFLAGS) -c $< -o $@

tests/test_table$(EXE): tests/test_table.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

tests/bench$(EXE): tests/bench.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

examples/demo$(EXE): examples/demo.c $(LIB)
	$(CC) $(CFLAGS) $< $(LIB) $(LDFLAGS) -o $@

demo: examples/demo$(EXE)
	./examples/demo$(EXE)

test: tests/test_table$(EXE)
	./tests/test_table$(EXE)

bench: tests/bench$(EXE)
	./tests/bench$(EXE) 1000000

clean:
	rm -f $(LIB) $(OBJS) src/plat_win32.o tests/test_table$(EXE) tests/bench$(EXE) examples/demo$(EXE) *.dat *.idx.* *.bak

.PHONY: all test bench demo clean
