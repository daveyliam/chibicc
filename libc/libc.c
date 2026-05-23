#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define __HEAP_SIZE (64 * 1024 * 1024)
uint8_t __heap_base[__HEAP_SIZE];
size_t __heap_top = 0;

// stdlib.h

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

unsigned long strtoul(const char *nptr, char **endptr, int base) {
  if (base < 0 || base == 1 || base > 16) {
    if (endptr) {
      *endptr = (char *)nptr;
    }
    return 0;
  }
  int c = *nptr;
  // Skip leading spaces.
  while (isspace(c)) {
    nptr++;
    c = *nptr;
  }
  // Read plus or minus.
  bool is_negative = false;
  if (c == '+' || c == '-') {
    is_negative = (c == '-');
    nptr++;
    c = *nptr;
  }
  // Read prefix.
  bool has_leading_zero = false;
  if (c == '0') {
    has_leading_zero = true;
    nptr++;
    c = *nptr;
  }
  if (base == 0 || base == 16) {
    if (has_leading_zero && (c == 'x' || c == 'X')) {
      base = 16;
      nptr++;
    }
  }
  if (base == 0 || base == 2) {
    if (has_leading_zero && (c == 'b' || c == 'B')) {
      base = 2;
      nptr++;
    }
  }
  if (base == 0 && has_leading_zero) {
    base = 8;
    nptr++;
  }
  if (base == 0) {
    base = 10;
  }
  // Read digits.
  unsigned long val = 0;
  while (1) {
    c = *nptr;
    int digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      break;
    }
    if (digit >= base) {
      break;
    }
    val = (val * base) + digit;
    nptr++;
  }
  // TODO : check for overflow and if so, set val to ULONG_MAX.
  if (is_negative) {
    val = -val;
  }
  if (endptr) {
    *endptr = (char *)nptr;
  }
  return val;
}

// unistd.h

ssize_t read(int fd, void *buf, size_t count) {
  return __builtin_syscall3(SYS_read, fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count) {
  return __builtin_syscall3(SYS_write, fd, buf, count);
}

int close(int fd) { return __builtin_syscall3(SYS_close, fd, 0, 0); }

int openat(int dirfd, const char *path, int flags, int mode) {
  return __builtin_syscall6(SYS_openat, dirfd, path, flags, mode, 0, 0);
}

int fchmodat(int dirfd, const char *path, int mode, int flags) {
  return __builtin_syscall6(SYS_fchmodat, dirfd, path, mode, flags, 0, 0);
}

_Noreturn void _exit(int status) {
  // TODO : should be SYS_exit_group?
  __builtin_syscall3(SYS_exit, status, 0, 0);
}

_Noreturn void exit(int status) {
  // Note : exit usually calls atexit and on_exit handlers.
  _exit(status);
}

// string.h

void *memset(void *s, int c, size_t n) {
  uint8_t *p = s;
  uint8_t c2 = (uint8_t)c;
  for (; n; n--) {
    *p++ = c2;
  }
  return s;
}

void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *p = dst;
  for (; n; n--) {
    *p++ = *(uint8_t *)src++;
  }
  return dst;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  for (; n; n--) {
    uint8_t b1 = *(uint8_t *)s1++;
    uint8_t b2 = *(uint8_t *)s2++;
    int diff = b1 - b2;
    if (diff) {
      return diff;
    }
  }
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  for (; n; n--) {
    int c2 = *(char *)s;
    if (c2 == 0) {
      return NULL;
    }
    if (c2 == c) {
      break;
    }
    s++;
  }
  return (void *)s;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (*s) {
    s++;
    n++;
  }
  return n;
}

char *strdup(const char *s) {
  size_t n = strlen(s);
  char *s2 = calloc(n + 1, 1);
  memcpy(s2, s, n);
  return s2;
}

char *strndup(const char *s, size_t n) {
  size_t n2 = strlen(s);
  if (n2 < n) {
    n = n2;
  }
  char *s2 = calloc(n + 1, 1);
  memcpy(s2, s, n);
  return s2;
}

char *strncpy(char *dst, const char *src, size_t n) {
  char *p = dst;
  for (; n; n--) {
    char c = *src++;
    *p++ = c;
  }
  return dst;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n) {
    unsigned char b1 = *(unsigned char *)s1;
    unsigned char b2 = *(unsigned char *)s2;
    int diff = b1 - b2;
    if (diff) {
      return diff;
    }
    if (!b1 || !b2) {
      break;
    }
    s1++;
    s2++;
    n--;
  }
  return 0;
}

int strcmp(const char *s1, const char *s2) {
  while (1) {
    unsigned char b1 = *(unsigned char *)s1;
    unsigned char b2 = *(unsigned char *)s2;
    int diff = b1 - b2;
    if (diff) {
      return diff;
    }
    if (!b1 || !b2) {
      break;
    }
    s1++;
    s2++;
  }
  return 0;
}

char *strchr(const char *s, int c) {
  while (1) {
    int c2 = *s;
    if (c2 == 0) {
      return NULL;
    }
    if (c2 == c) {
      break;
    }
    s++;
  }
  return (char *)s;
}

char *strstr(const char *haystack, const char *needle) {
  if (*needle == 0) {
    return (char *)haystack;
  }
  int needle_len = strlen(needle);
  while (*haystack++) {
    if (strncmp(haystack, needle, needle_len) == 0) {
      return (char *)haystack;
    }
  }
  return NULL;
}

// stdarg.h

void *__va_arg(__va_elem *ap, int sz, int align) {
  void *p = ap->arg_area;
  ap->arg_area = (void *)(((uintptr_t)p + sz + 7) & ~7);
  return p;
}

// stdio.h

static FILE stdin_file = {.fd = 0};
static FILE stdout_file = {.fd = 1};
static FILE stderr_file = {.fd = 2};
FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

int fputs(const char *s, FILE *f) { return write(f->fd, s, strlen(s)); }
int puts(const char *s) { return fputs(s, stdout); }

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

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  const char *start = fmt;
  int n = 0;
  while (1) {
    char c = *fmt;
    if (c == 0) {
      if (start != fmt) {
        n += write(f->fd, start, fmt - start);
      }
      break;
    } else if (c == '%') {
      if (start != fmt) {
        n += write(f->fd, start, fmt - start);
      }
      fmt += 1;
      c = *fmt;
      if (c == 0) {
        break;
      }
      if (c == '%') {
        n += write(f->fd, &c, 1);
        fmt += 1;
      } else if (c == 'd') {
        fmt += 1;
        int val = va_arg(ap, int);
        char buf[32] = {0};
        format_decimal(buf, val);
        n += puts(buf);
      } else if (c == 's') {
        fmt += 1;
        char *val = va_arg(ap, char *);
        n += puts(val);
      } else {
        fmt += 1;
      }
      start = fmt;
    } else {
      fmt += 1;
    }
  }
  // TODO : check write results and add calculate n.
  return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

static inline size_t __snprintf_write(char *buf, size_t size, size_t pos, char c) {
  if (buf != NULL && (pos < size)) {
    buf[pos] = c;
  }
  return pos + 1;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  int pos = 0;
  while (1) {
    char c = *fmt;
    if (c == 0) {
      break;
    } else if (c == '%') {
      fmt += 1;
      c = *fmt;
      if (c == 0) {
        break;
      }
      if (c == '%') {
        pos = __snprintf_write(buf, size, pos, c);
        fmt += 1;
      } else if (c == 'd') {
        fmt += 1;
        int val = va_arg(ap, int);
        char buf2[32] = {0};
        int n2 = format_decimal(buf2, val);
        for (int i = 0; i < n2; i++) {
          pos = __snprintf_write(buf, size, pos, buf2[i]);
        }
      } else if (c == 's') {
        fmt += 1;
        char *buf2 = va_arg(ap, char *);
        while (*buf2) {
          pos = __snprintf_write(buf, size, pos, *buf2);
          buf2 += 1;
        }
      } else {
        fmt += 1;
      }
    } else {
      pos = __snprintf_write(buf, size, pos, c);
      fmt += 1;
    }
  }
  __snprintf_write(buf, size, pos, 0);
  return pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, SIZE_MAX, fmt, ap);
  va_end(ap);
  return n;
}

// assert.h

void __assert_fail(const char *s, const char *filename, int line, const char *funcname) {
  fprintf(stderr, "assertion failed: %s:%d: %s\n", filename, line, s);
  exit(1);
}

// ctype.h

int isspace(int c) { return c == ' ' || (unsigned)c - '\t' < 5; }

int isdigit(int c) { return (unsigned)c - '0' < 10; }

int isxdigit(int c) { return isdigit(c) || ((unsigned)c | 32) - 'a' < 6; }

int isalpha(int c) { return ((unsigned)c | 32) - 'a' < 26; }

int isalnum(int c) { return isalpha(c) || isdigit(c); }

int isgraph(int c) { return ((unsigned)c - 0x21) < 0x5e; }

int ispunct(int c) { return isgraph(c) && !isalnum(c); }

// _start.

int main(int argc, char **argv, char **envp);

void _start(void) {

  // Load command line arguments.
  // fp + 0  : saved fp
  // fp + 8  : argc
  // fp + 16 : argv[0]
  // fp + 24 : argv[1]
  // fp + ?  : argv[n]
  // fp + ?  : 0
  // fp + ?  : envp[0]
  // fp + ?  : envp[n]
  // fp + ?  : 0

  void **fp = __builtin_get_fp();
  int argc = *(int *)(fp + 1);
  char **argv = (char **)(fp + 2);
  char **envp = (char **)(fp + argc + 3);

  _exit(main(argc, argv, envp));
}
