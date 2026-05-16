CFLAGS:=-std=c11 -g -O1 -fno-omit-frame-pointer -fno-common -Wall -Werror -fPIC

CFLAGS += -fsanitize=address

SRCS:=$(wildcard *.c)
OBJS:=$(patsubst %.c,build/%.o,$(SRCS))

TEST_SRCS:=$(filter-out test/common.c, $(wildcard test/*.c))
TESTS:=$(patsubst test/%.c,build/test/%.exe,$(TEST_SRCS))
TESTS_STAGE2:=$(patsubst test/%.c,build/stage2/test/%.exe,$(TEST_SRCS))

CC:=clang

$(shell mkdir -p build/test build/stage2/test)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

build/test/%.exe: test/%.c chibicc test/common.c libc/libc.c
	./chibicc -Itest -DTEST_NAME='"$<"' -o build/test/$*.exe \
	  $< test/common.c libc/libc.c

test: $(TESTS) | chibicc
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
	test/driver.sh ./chibicc

# test-all: test test-stage2

# Stage 2

build/stage2/chibicc: $(SRCS) | chibicc libc/libc.c
	./chibicc -o $@ libc/libc.c $^

build/stage2/test/%.exe: test/%.c build/stage2/chibicc test/common.c libc/libc.c 
	./build/stage2/chibicc -Iinclude -Itest -DTEST_NAME='"$<"' -o $@ \
	  $< test/common.c libc/libc.c

test-stage2: $(TESTS_STAGE2) | build/stage2/chibicc libc/libc.c
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
	test/driver.sh ./build/stage2/chibicc

# Misc.

format:
	for f in $(SRCS) libc/libc.c include/*.h test/common.c test/test.h; do \
	  clang-format -i "$$f"; \
	done

clean:
	rm -rf chibicc tmp* build

.PHONY: test test-stage2 clean format
