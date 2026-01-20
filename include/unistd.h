#ifndef __UNISTD_H
#define __UNISTD_H

#include <stddef.h>
#include <stdint.h>

#define WASM_PAGE_SIZE 65536

typedef long ssize_t;

void *sbrk(intptr_t increment);
ssize_t write(int fd, const void *buf, size_t count);

#endif
