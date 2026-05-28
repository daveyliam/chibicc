CFLAGS:=-std=c11 -g -O1 -fno-omit-frame-pointer -fno-common -Wall -Werror -fPIC

CFLAGS+=-fsanitize=address

SRCS:=$(wildcard *.c)
OBJS:=$(patsubst %.c,build/%.o,$(SRCS))

TEST_SRCS:=$(filter-out test/common.c, $(wildcard test/*.c))
TESTS:=$(patsubst test/%.c,build/test/%.exe,$(TEST_SRCS))
TESTS_STAGE2:=$(patsubst test/%.c,build/stage2/test/%.exe,$(TEST_SRCS))

CC:=clang

$(shell mkdir -p build/test build/stage2/test build/stage3)

# Stage 1.
# Compile chibicc with clang and test.

build/chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

build/test/%.exe: test/%.c build/chibicc test/common.c libc/libc.c
	./build/chibicc -Iinclude -Itest -DTEST_NAME='"$<"' -o build/test/$*.exe \
	  $< test/common.c libc/libc.c

test: $(TESTS) | build/chibicc
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
	test/driver.sh ./build/chibicc

# Stage 2.
# Build chibicc using the clang compiled chibicc, and test.

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

# Stage 3.
# Build chibicc using the chibicc compiled chibicc, and make sure it matches stage 2.

build/stage3/chibicc: $(SRCS) | build/stage2/chibicc libc/libc.c
	./build/stage2/chibicc -Iinclude -o $@ libc/libc.c $^

test-stage3: build/stage2/chibicc build/stage3/chibicc
	if ! cmp -s $^ ; then \
	  echo "files do not match: " $^ ; \
	  exit 1; \
	fi; \
	echo "files match, we are self-bootstrapping!"


test-all: test test-stage2 test-stage3

# Misc.

format:
	for f in $(SRCS) libc/libc.c include/*.h test/common.c test/test.h; do \
	  clang-format -i "$$f"; \
	done

clean:
	rm -rf tmp* build

.PHONY: test test-stage2 test-stage3 clean format
