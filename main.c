#include "header.h"

// vars
int maxfd = 3;
fd_set rin, win, ein;
fd_set rout, wout, eout;
handlers rha={[0 ... FD_SETSIZE-1]=0}, wha={[0 ... FD_SETSIZE-1]=0}, eha={[0 ... FD_SETSIZE-1]=0};
int nfound;
// end vars

void(*cmds[256])()={[0 ... 255]=0};



void register_in(int fd, handler h) {
    if(fd+1>maxfd) { maxfd = fd+1; }
    FD_SET(fd, &rin);
    rha[fd]=h;
}
void register_out(int fd, handler h) {
    if(fd+1>maxfd) { maxfd = fd+1; }
    FD_SET(fd, &win);
    wha[fd]=h;
}
void register_except(int fd, handler h) {
    if(fd+1>maxfd) { maxfd = fd+1; }
    FD_SET(fd, &ein);
    eha[fd]=h;
}
void unregister(int fd) {
    FD_CLR(fd, &rin);
    FD_CLR(fd, &win);
    FD_CLR(fd, &ein);
}


int main(int argc, char* argv[]) {

    setup_trace(argc, argv);

    FD_ZERO(&rin);
    FD_ZERO(&win);
    FD_ZERO(&ein);

    register_in(0, handle_stdin);
    register_except(0, handle_stdin_exception);

    fcntl(0, F_SETFL, O_NONBLOCK);
    
    cmds_init();
    init_server_socket();


    for(;;) {
	int i;

	rout=rin, wout=win, eout=ein;
	nfound = select(maxfd, &rout, &wout, &eout, 0);
	
	if (nfound==-1) {
	    perror("select");
	}

	for (i=0; i<maxfd; ++i) {
	    if(FD_ISSET(i, &eout)) {
		--nfound;
		FD_CLR(i, &ein); 
		eha[i](i);
	    }
	    if(FD_ISSET(i, &wout)) {
		--nfound;
		FD_CLR(i, &win);
		wha[i](i);
	    }
	    if(FD_ISSET(i, &rout)) {
		--nfound;
		FD_CLR(i, &rin);
		rha[i](i);
	    }
	}

	if(nfound) {
	    fprintf(stderr, "nfound=%d\n", nfound);
	}

    }

    return 0;
}
