#ifndef __UNISTD_H
#define __UNISTD_H

#include <stddef.h>
#include <stdnoreturn.h>
#include <sys/types.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 64
#define O_TRUNC 512

#define MAP_FAILED (-1)

#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define AT_FDCWD (-100)

ssize_t write(int fd, const void *buf, size_t count);
_Noreturn void _exit(int status);

#endif
