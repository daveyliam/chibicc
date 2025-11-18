#include "test.h"

// Clang: error: restrict requires a pointer or reference ('int' is invalid)
// _Noreturn noreturn_fn(int restrict x) {
//   exit(0);
// }

void funcy_type(int arg[restrict static 3]) {}

int main() {
  // Clang: error: type specifier missing, defaults to 'int'; ISO C99 and later
  // do not support implicit int [-Wimplicit-int]
  // { volatile x; }
  { int volatile x; }
  { volatile int x; }
  { volatile int volatile volatile x; }
  { int volatile * volatile volatile x; }
  // Clang: error: type specifier missing, defaults to 'int'; ISO C99 and later
  // do not support implicit int [-Wimplicit-int]
  // { auto ** restrict __restrict __restrict__ const volatile *x; }

  printf("OK\n");
  return 0;
}
