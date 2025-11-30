CFLAGS:=-std=c11 -g -fno-common -Wall -Werror -fPIC

SRCS:=$(wildcard *.c)
OBJS:=$(patsubst %.c,build/%.o,$(SRCS))

TEST_SRCS:=$(filter-out test/common.c, $(wildcard test/*.c))
TESTS:=$(patsubst test/%.c,build/test/%.exe,$(TEST_SRCS))

CC:=clang

$(shell mkdir -p build/test)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

build/test/%.o: test/%.c chibicc
	./chibicc -Iinclude -Itest -S -o build/test/$*.wat $<
	wat2wasm build/test/$*.wat -o build/test/$*.wasm
	wasm-validate build/test/$*.wasm
	cp build/test/$*.wasm $@
#	./chibicc -Iinclude -Itest -c -o $@ $<

build/test/%.exe: build/test/%.o build/test/common.o
	$(CC) -pthread -o $@ $^

test: $(TESTS)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./chibicc

# test-all: test test-stage2

# Stage 2

# stage2/chibicc: $(OBJS:%=stage2/%)
# 	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# stage2/%.o: chibicc %.c
# 	mkdir -p stage2/test
# 	./chibicc -c -o $(@D)/$*.o $*.c

# stage2/test/%.exe: stage2/chibicc test/%.c
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
