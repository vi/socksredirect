#include "header.h"

void handle_stdin(int fd) {
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
    register_in(fd, handle_stdin);
}

void handle_stdin_exception(int fd) {
    fprintf(stderr, "Exception at stdin\n");
    exit(1);
}
