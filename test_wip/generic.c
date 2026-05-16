#include "test.h"

int test_main() {
  ASSERT(1, _Generic(100.0, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(2, _Generic((int *)0, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(2, _Generic((int[3]){}, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(3, _Generic(100, double: 1, int *: 2, int: 3, float: 4));
  // Clang: error: invalid digit 'f' in decimal constant
  // ASSERT(4, _Generic(100f, double: 1, int *: 2, int: 3, float: 4));
  ASSERT(4, _Generic(100.0f, double: 1, int *: 2, int: 3, float: 4));

  return 0;
}
