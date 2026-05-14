void _start(void) {
    __builtin_syscall3(1, 1, "write.c: ok\n", 12);
    __builtin_syscall3(60, 0, 0, 0);
}
