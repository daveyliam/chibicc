#include "test.h"

int test_main() {
    ASSERT(123, strtoul("123", NULL, 10));
    ASSERT(-123, strtoul("-123", NULL, 10));
    ASSERT(255, strtoul("ff", NULL, 16));
    ASSERT(13, strtoul("1101", NULL, 2));
    ASSERT(255, strtoul("0xff", NULL, 16));
    ASSERT(255, strtoul("0xFF", NULL, 16));
    ASSERT(255, strtoul("0xff", NULL, 0));

    const char *s = "0xffffffffffffffffUL";
    char *endptr = NULL;
    ASSERT(1, (strtoul(s, &endptr, 16) == 0xffffffffffffffffUL) ? 1 : 0);
    ASSERT(18, (int)(endptr - s));

    return 0;
}
