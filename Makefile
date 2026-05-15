CFLAGS:=-std=c11 -g -O1 -fno-omit-frame-pointer -fno-common -Wall -Werror -fPIC

CFLAGS += -fsanitize=address

SRCS:=$(wildcard *.c)
OBJS:=$(patsubst %.c,build/%.o,$(SRCS))

TEST_SRCS:=$(filter-out test/common.c, $(wildcard test/*.c))
TESTS:=$(patsubst test/%.c,build/test/%.exe,$(TEST_SRCS))

EARLY_TEST_SRCS:=$(wildcard test_early/*.c)
EARLY_TESTS:=$(patsubst test_early/%.c,build/test_early/%.exe,$(EARLY_TEST_SRCS))

CC:=clang

$(shell mkdir -p build/test build/test_early)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

build/test_early/%.exe: test_early/%.c chibicc
	ASAN_OPTIONS="detect_leaks=0" \
	./chibicc -o build/test_early/$*.exe $<

test_early: $(EARLY_TESTS)
	ASAN_OPTIONS="detect_leaks=0" \
	./chibicc -o build/test_early/simple.exe test_early/simple.c
	./build/test_early/simple.exe; \
	if [[ $$? -eq 30 ]]; then \
	  echo simple.c: ok; \
	else \
	  echo simple.c: fail; \
	  exit 1; \
	fi
	./build/test_early/write.exe

build/test/%.exe: test/%.c chibicc
	ASAN_OPTIONS="detect_leaks=0" \
	./chibicc -Itest -DTEST_NAME='"$<"' -o build/test/$*.exe \
	  $< test/common.c libc/libc.c

test: $(TESTS)
	fail=0; \
	for i in $^; do \
	  if ! ./$$i ; then \
	    fail=$$((fail + 1)); \
	  fi; \
	done; \
	if [[ $$fail -ne 0 ]]; then \
	  echo "$$fail tests failed"; \
	  exit 1; \
	fi
	ASAN_OPTIONS="detect_leaks=0" \
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

format:
	for f in $(SRCS); do \
	  clang-format -i "$$f"; \
	done

clean:
	rm -rf chibicc tmp* build

.PHONY: test clean test-stage2 format
