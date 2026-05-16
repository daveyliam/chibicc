#ifndef __STDARG_H
#define __STDARG_H

typedef struct {
  void *arg_area;
} __va_elem;

typedef __va_elem va_list[1];

#define va_start(ap, last) \
  do { \
    *(ap) = *(__va_elem *)__va_area__; \
  } while (0)

#define va_end(ap)

void *__va_arg(__va_elem *ap, int sz, int align);

#define va_arg(ap, ty) (*(ty *)__va_arg(ap, sizeof(ty), _Alignof(ty)))

#define va_copy(dest, src) ((dest)[0] = (src)[0])

#define __GNUC_VA_LIST 1
typedef va_list __gnuc_va_list;

#endif
