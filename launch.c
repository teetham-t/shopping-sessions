#include <unistd.h>
int main(int argc, char **argv) {
    (void)argc;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("x86-64-v3")) execv("/sessions-v3", argv);
#endif
    execv("/sessions-portable", argv);
    return 127;
}
