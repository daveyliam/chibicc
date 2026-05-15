typedef struct { char a, b[]; } T65;
T65 g65 = {'f','o','o',0};
T65 g66 = {'f','o','o','b','a','r',0};

void _start(void) {
    // int a = 10;
    // int b = 20;
    // __builtin_syscall3(60, a + b, 0, 0);
    __builtin_syscall3(60, U'\xffffffff'>>31, -1U>>31, 0);
}
