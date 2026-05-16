#ifndef __UNISTD_H
#define __UNISTD_H

#include <stddef.h>
#include <stdnoreturn.h>
#include <sys/types.h>

#define MAP_FAILED (-1)

#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
_Noreturn void _exit(int status);

#endif
