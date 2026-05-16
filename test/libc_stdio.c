#include "test.h"

int test_main() {
    char buf[64] = {};
    int n;

    n = snprintf(buf, sizeof(buf), "%s/%s", "foo", "bar");
    ASSERT(0, strcmp(buf, "foo/bar"));
    ASSERT(7, n);

    n = snprintf(buf, sizeof(buf), "%d", 1234);
    ASSERT(0, strcmp(buf, "1234"));
    ASSERT(4, n);
    ASSERT(4, snprintf(NULL, 0, "%d", 1234));

    return 0;
}
