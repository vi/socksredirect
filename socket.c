#include "header.h"

void init_server_socket() {
    int ss = socket(PF_INET, SOCK_STREAM, 0);
    if	(ss==-1) {
	perror("socket");
	exit(1);
    }

    int opt=1;
    setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(ss, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family=PF_INET;
    sa.sin_port=htons(1236);
    inet_aton("0.0.0.0", &sa.sin_addr);
    if(-1==bind(ss, (struct sockaddr*) &sa, sizeof sa)) {
	close(ss);
	perror("bind");
	exit(1);
    }
    if(-1==listen(ss, 0)) {
	close(ss);
	perror("listen");
	exit(1);
    }
    register_in(ss, ss_onconnect);
    register_except(ss, ss_except);
}


void ss_onconnect(int ss) {
    struct sockaddr_in sa;
    int len = sizeof sa;
    int s = accept(ss, (struct sockaddr*) &sa, &len);

    write(s, "HW\n", 3);
    close(s);

    printf("%s:%d\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

    register_in(ss, ss_onconnect);
}

void ss_except(int ss) {
    fprintf(stderr, "Caught exception at server socket\n");
    exit(2);
}
