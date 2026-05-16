#ifndef __STDASSERT_H
#define __STDASSERT_H

#undef assert

#ifdef NDEBUG
#define assert(x) (void)0
#else
#define assert(x) \
  if (!x) { \
    __assert_fail(#x, __FILE__, __LINE__, __func__); \
  }
#endif

void __assert_fail(const char *s, const char *filename, int line, const char *funcname);

#endif
