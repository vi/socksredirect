#include "header.h"
#include <signal.h>

// vars
// end vars

void(*cmds[256])()={[0 ... 255]=0};

void sigpipe() {
    fprintf(stderr, "SIGPIPE\n");
}

int main(int argc, char* argv[]) {

    setup_trace(argc, argv);    
    {
	struct sigaction sa = {sigpipe};
	sigaction(SIGPIPE, &sa, NULL);
    }

    event_init();
    init_stdin();
    init_server_socket();
    event_dispatch();

    return 0;
}
