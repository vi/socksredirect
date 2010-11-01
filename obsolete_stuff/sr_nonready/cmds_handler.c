#include "header.h"

void handle_stdin(int fd, short event, void *arg) {
    unsigned char c=0;
    while (scanf("%c", &c)==1) {
	if (cmds[c]) {
	    cmds[c]();
	} else {
	    if(c>32) {
		fprintf(stderr, "Undefined command %c\n", c);
	    }
	}
    }
    if(c==0) {
	exit(0);
    }
    event_add((struct event*)arg, NULL);
}

struct event stdinEvent;
void init_stdin() {
    cmds_init();
    fcntl(0, F_SETFL, O_NONBLOCK);
    event_set(&stdinEvent, 0, EV_READ, handle_stdin, &stdinEvent);
    event_add(&stdinEvent, NULL);
}

void handle_stdin_exception(int fd) {
    fprintf(stderr, "Exception at stdin\n");
    exit(1);
}
