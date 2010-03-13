#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <string.h>

const char* argv0;            
        
void print_trace() {
    write(2, "*** BACKTRACE ***\n", 19);

    char buf[30];
    sprintf(buf, "%d", getpid());
    //sprintf(buf, "gdb -batch -n -ex \"bt full\" %s %d", argv0, getpid());
    int q = fork();
    if(!q) {
	execlp("gdb", "gdb", "-batch", "-n", "-ex", "bt full", argv0, buf, NULL);
    } else {
	wait(q);
    }
    //system(buf);
    write(2, "*** END OF BACKTRACE ***\n", 26);
}

void bad_signal() {
    print_trace();
    exit(2);
}

void setup_trace(int argc, char* argv[]) {
    argv0=argv[0];
    struct sigaction sa = {bad_signal};
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

