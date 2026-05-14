#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define __HEAP_SIZE (64 * 1024 * 1024)
uint8_t __heap_base[__HEAP_SIZE];
size_t __heap_top = 0;

void *memset(void *s, int c, size_t n) {
    uint8_t c2 = (uint8_t) c;
    for (int i = 0; i < n; i++) {
        ((uint8_t*)s)[i] = c2;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    for (int i = 0; i < n; i++) {
        ((uint8_t*)dest)[i] = ((uint8_t*)src)[i];
    }
    return dest;
}

void *malloc(size_t size) {
    // Bump allocator.
    if (__heap_top + size > __HEAP_SIZE) {
        return NULL;
    }
    void *p = __heap_base + __heap_top;
    __heap_top = (__heap_top + size + 7) & ~7;
    return p;
}

void free(void *p) {
    // TODO.
}

void *calloc(size_t n, size_t size) {
    void *p = malloc(n * size);
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, n * size);
    return p;
}

ssize_t write(int fd, const void *buf, size_t count) {
    ssize_t err = __builtin_syscall3(SYS_write, fd, buf, count);
    return err;
}

_Noreturn void _exit(int status) {
    // TODO : should be SYS_exit_group?
    __builtin_syscall3(SYS_exit, status, 0, 0);
}

_Noreturn void exit(int status) {
    // Note : exit usually calls atexit and on_exit handlers.
    _exit(status);
}

int memcmp(const void *s1, const void *s2, size_t n) {
    for (size_t i = 0; i < n; i += 1) {
        unsigned char b1 = ((unsigned char *) s1)[i];
        unsigned char b2 = ((unsigned char *) s2)[i];
        int diff = b1 - b2;
        if (diff) {
            return diff;
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s) {
        s += 1;
        n += 1;
    }
    return n;
}

int strcmp(const char *s1, const char *s2) {
    while (1) {
        unsigned char b1 = *(unsigned char *) s1;
        unsigned char b2 = *(unsigned char *) s2;
        int diff = b1 - b2;
        if (diff) {
            return diff;
        }
        if (!b1 || !b2) {
            break;
        }
    }
    return 0;
}

void *__va_arg(__va_elem *ap, int sz, int align) {
  void *p = ap->arg_area;
  ap->arg_area = (void *)(((uintptr_t)p + sz + 7) & ~7);
  return p;
}

int puts(const char *s) {
    return write(1, s, strlen(s));
}

static int format_decimal(char *buf, int val) {
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }
    int pos = 0;
    if (val < 0) {
        buf[pos] = '-';
        pos += 1;
        val = -val;
    }
    int val2 = val;
    while (val) {
        pos += 1;
        val /= 10;
    }
    int endpos = pos;
    val = val2;
    while (val) {
        int digit = val % 10;
        pos -= 1;
        buf[pos] = '0' + digit;
        val /= 10;
    }
    return endpos;
}

int printf(const char *fmt, ...) {
    const char *start = fmt;
    int n = 0;
    va_list ap;
    va_start(ap, fmt);
    while (1) {
        char c = *fmt;
        if (c == 0) {
            if (start != fmt) {
                write(1, start, fmt - start);
            }
            break;
        }
        else if (c == '%') {
            if (start != fmt) {
                write(1, start, fmt - start);
            }
            fmt += 1;
            c = *fmt;
            if (c == 0) {
                break;
            }
            if (c == '%') {
                write(1, &c, 1);
                fmt += 1;
            }
            else if (c == 'd') {
                fmt += 1;
                int val = va_arg(ap, int);
                char buf[32] = {0};
                format_decimal(buf, val);
                puts(buf);
            }
            else if (c == 's') {
                fmt += 1;
                char *val = va_arg(ap, char *);
                puts(val);
            }
            else {
                fmt += 1;
            }
            start = fmt;
        }
        else {
            fmt += 1;
        }
    }
    va_end(ap);
    // TODO : check write results and add calculate n.
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    int pos = 0;
    va_list ap;
    va_start(ap, fmt);
    while (1) {
        char c = *fmt;
        if (c == 0) {
            break;
        }
        else if (c == '%') {
            fmt += 1;
            c = *fmt;
            if (c == 0) {
                break;
            }
            if (c == '%') {
                buf[pos] = c;
                pos += 1;
                fmt += 1;
            }
            else if (c == 'd') {
                fmt += 1;
                int val = va_arg(ap, int);
                int n2 = format_decimal(&buf[pos], val);
                pos += n2;
            }
            else if (c == 's') {
                fmt += 1;
                char *buf2 = va_arg(ap, char *);
                while (*buf2) {
                    buf[pos] = *buf2;
                    pos += 1;
                    buf2 += 1;
                }
            }
            else {
                fmt += 1;
            }
        }
        else {
            buf[pos] = c;
            pos += 1;
            fmt += 1;
        }
    }
    va_end(ap);
    return pos;
}

int main(void);
void _start(void) {
    _exit(main());
}
