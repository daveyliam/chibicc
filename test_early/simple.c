void _start(void) {
    int a = 10;
    int b = 20;
    __builtin_syscall3(60, a + b, 0, 0);
}
