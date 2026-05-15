#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(x, y) assert(x, y, #y)

void assert(int expected, int actual, const char *code);
