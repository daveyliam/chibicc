#include "test.h"

int test_main() {
    ASSERT(0, strcmp("foo", "foo"));
    ASSERT(4, strcmp("foo", "bar"));
    ASSERT(-4, strcmp("bar", "foo"));

    return 0;
}
