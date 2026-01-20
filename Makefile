CFLAGS:=-std=c11 -g -O1 -fno-omit-frame-pointer -fno-common -Wall -Werror -fPIC

# CFLAGS += -fsanitize=address

SRCS:=$(wildcard *.c)
OBJS:=$(patsubst %.c,build/%.o,$(SRCS))

TEST_SRCS:=$(filter-out test/common.c, $(wildcard test/*.c))
TESTS:=$(patsubst test/%.c,build/test/%.wasm,$(TEST_SRCS))

CC:=clang

$(shell mkdir -p build/test)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

build/test/%.wasm: test/%.c chibicc
	./chibicc -Itest -o build/test/$*.wat $< test/common.c libc/libc.c
	wat2wasm --debug-names --debug-parser build/test/$*.wat -o $@
	wasm-validate $@

test: $(TESTS)
	for i in $^; do echo $$i; wasmtime ./$$i || exit 1; echo; done
	test/driver.sh ./chibicc

# test-all: test test-stage2

# Stage 2

# stage2/chibicc: $(OBJS:%=stage2/%)
# 	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# stage2/%.o: chibicc %.c
# 	mkdir -p stage2/test
# 	./chibicc -c -o $(@D)/$*.o $*.c

# stage2/test/%.wasm: stage2/chibicc test/%.c
# 	mkdir -p stage2/test
# 	./stage2/chibicc -Iinclude -Itest -c -o stage2/test/$*.o test/$*.c
# 	$(CC) -pthread -o $@ stage2/test/$*.o -xc test/common

# test-stage2: $(TESTS:test/%=stage2/test/%)
# 	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
# 	test/driver.sh ./stage2/chibicc

# Misc.

clean:
	rm -rf chibicc tmp* build

.PHONY: test clean test-stage2
