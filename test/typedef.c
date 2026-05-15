#include "test.h"

typedef int MyInt, MyInt2[4];
typedef int;

int test_main() {
  ASSERT(1, ({ typedef int t; t x=1; x; }));
  ASSERT(1, ({ typedef struct {int a;} t; t x; x.a=1; x.a; }));
  // Clang: error: redefinition of 't' as different kind of symbol
  // ASSERT(1, ({ typedef int t; t t=1; t; }));
  ASSERT(1, ({ typedef int t; t t1=1; t1; }));
  ASSERT(2, ({ typedef struct {int a;} t; { typedef int t; } t x; x.a=2; x.a; }));
  // Clang: error: type specifier missing, defaults to 'int'; ISO C99 and
  // later do not support implicit int [-Wimplicit-int]
  // ASSERT(4, ({ typedef t; t x; sizeof(x); }));
  ASSERT(4, ({ typedef int t; t x; sizeof(x); }));
  ASSERT(3, ({ MyInt x=3; x; }));
  ASSERT(16, ({ MyInt2 x; sizeof(x); }));

  return 0;
}
