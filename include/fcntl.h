#ifndef __STDFCNTL_H
#define __STDFCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 64
#define O_TRUNC 512

#define AT_FDCWD (-100)

int openat(int dirfd, const char *path, int flag, int mode);
int fchmodat(int dirfd, const char *path, int mode, int flags);

#endif
