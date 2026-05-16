#ifndef __STDIO_H
#define __STDIO_H

#include "stdarg.h"
#include "stddef.h"

typedef struct FILE FILE;
struct FILE {
  int fd;
};

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int fputs(const char *s, FILE *f);
int puts(const char *s);
int vfprintf(FILE *f, const char *format, va_list ap);
int fprintf(FILE *f, const char *format, ...);
int printf(const char *format, ...);
int vsnprintf(char *buf, size_t size, const char *format, va_list ap);
int snprintf(char *buf, size_t size, const char *format, ...);
int sprintf(char *buf, const char *format, ...);

#endif
