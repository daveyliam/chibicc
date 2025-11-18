#pragma once

#ifdef PRAGMA_ONCE_TEST
#define X 123
#else
#undef X
#define X 456
#endif

#define PRAGMA_ONCE_TEST
