#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct IOVec {
  void *data;
  size_t length;
} IOVec;

int fd_write(int fd, IOVec *iovs, size_t iovs_len, size_t *nwritten);

_Noreturn void proc_exit(int rval); 

void *__brk_addr = (void *) -1;

static uintptr_t get_num_pages(uintptr_t n_bytes) {
  return (n_bytes + WASM_PAGE_SIZE - 1) / WASM_PAGE_SIZE;
}

void *sbrk(intptr_t increment) {
    if (__brk_addr == (void *) -1) {
        __brk_addr = __builtin_memory_size() * WASM_PAGE_SIZE;
    }
    if (increment < 0) {
        return (void *) -1;
    }
    if (increment == 0) {
        return __brk_addr;
    }
    void *cur_brk_addr = __brk_addr;
    uintptr_t cur_pages = get_num_pages((uintptr_t) __brk_addr);
    void *new_brk_addr = (uintptr_t) __brk_addr + (uintptr_t) increment;
    uintptr_t new_pages = get_num_pages((uintptr_t) new_brk_addr);
    if (new_pages > cur_pages) {
        if (__builtin_memory_grow(new_pages - cur_pages) == -1) {
            return (void *) -1;
        }
    }
    __brk_addr = new_brk_addr;
    return cur_brk_addr;
}


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
    void *p = sbrk((intptr_t) size);
    if (p == (void *) -1) {
        return NULL;
    }
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
    IOVec vec = {
        .data = buf,
        .length = count,
    };
    int nwritten = 0;
    int err = fd_write(1, &vec, 1, &nwritten);
    if (err) {
        return -1;
    }
    return (ssize_t) nwritten;
}

_Noreturn void exit(int status) {
    proc_exit(status);
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
  if (align > 8) {
    p = ((unsigned long)p + 15) / 16 * 16;
  }
  ap->arg_area = ((unsigned long)p + sz + 7) / 8 * 8;
  return p;
}

int puts(const char *s) {
    return write(1, s, strlen(s));
}

static int format_decimal(char *buf, int val) {
    if (val == 0) {
        buf[0] = "0";
        return 1;
    }
    int pos = 0;
    if (val < 0) {
        buf[pos] = "-";
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
    proc_exit(main());
}
