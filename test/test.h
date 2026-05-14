#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define ASSERT(x, y) assert(x, y, #y)

void assert(int expected, int actual, char *code);
