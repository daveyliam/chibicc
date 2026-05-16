#ifndef __SYS_SYSCALL_H
#define __SYS_SYSCALL_H

#if defined(__x86_64__)
#  define SYS_read 0
#  define SYS_write 1
#  define SYS_close 3
#  define SYS_mmap 9
#  define SYS_exit 60
#  define SYS_openat 257
#  define SYS_fchmodat 268
#elif defined(__aarch64__) || defined(__arm64__)
#  define SYS_read 63
#  define SYS_write 64
#  define SYS_close 57
#  define SYS_mmap 222
#  define SYS_exit 93
#  define SYS_openat 56
#else
#  error "unsupported architecture"
#endif

#endif
