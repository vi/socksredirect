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

char buffer[1024];
char* errmsg[FD_SETSIZE];
int portmap[FD_SETSIZE];

void ss_onconnect(int ss) {
    struct sockaddr_in sa,da,ka;
    int len = sizeof sa;
    int s = accept(ss, (struct sockaddr*) &sa, &len);
    fcntl(s, F_SETFL, O_NONBLOCK);

    register_in(ss, ss_onconnect);

    register_in(s, s_ready);
    register_except(s, s_except);

    len = sizeof da;
    if (getsockopt(s, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr *)&da, &len) != 0) {
	s_err(s, "Unable to get destination address");
    }


    printf("%s:%d -> %s->%d\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), inet_ntoa(da.sin_addr), ntohs(da.sin_port));

    int cs = socket(PF_INET, SOCK_STREAM, 0);
    if (cs==-1) {
	s_err(s, "Unable to create client socket to connect to SOCKS server\n");
	return;
    }
    fcntl(cs, F_SETFL, O_NONBLOCK);
    memset(&ka, 0, sizeof ka);
    ka.sin_family=PF_INET;
    ka.sin_port=htons(1237);
    inet_aton("127.0.0.1", &ka.sin_addr);
    if(-1==connect(cs, (struct sockaddr*)&ka, sizeof ka)) {
	if (errno!=EWOULDBLOCK && errno!=EINPROGRESS) {
	    s_err(s, "Unable to connect to SOCKS server\n");
	    return;
	}
    }

    register_in(cs, s_ready);
    register_except(cs, s_except);
    portmap[cs]=s;
    portmap[s]=cs;  

}

void s_err(int s, char* msg) {
    unregister(s);
    unportmap(s);
    ssize_t wlen = write(s, msg, strlen(msg));
    if (wlen==-1) {
	if(errno==EWOULDBLOCK) {
	    errmsg[s]=msg;
	    register_out(s, s_err2);
	}
    }
    close(s);
};

void s_err2(int s) {
    write(s, errmsg[s], strlen(errmsg[s]));
    close(s);
};

void ss_except(int ss) {
    fprintf(stderr, "Caught exception at server socket\n");
    exit(2);
}

void s_ready(int s) {
    ssize_t len = read(s, buffer, sizeof buffer);
    if(!len || len==-1) {
	s_except(s);
    }
    if(portmap[s]) {
	int cs = portmap[s];
	register_in(s, s_ready);
    
	ssize_t wlen = write(cs, buffer, len);
	if (wlen==-1) {
	    if(errno==EWOULDBLOCK) {
		// TODO
	    } else {
		s_except(cs);
	    }
	}
	if (wlen<len) {
	    // TODO
	}
    } else {
	fprintf(stderr, "Nowhere to route data from socket %d\n", s);
	unregister(s);
	unportmap(s);
    }
}

void s_except(int s) {
    int cs = portmap[s];
    close(s);
    unregister(s);       
    unportmap(s);
    if(cs) {
	close(cs);
	unregister(cs);
	unportmap(cs);
    } else {
    }
}

void unportmap(int s) {
    int i;
    portmap[s]=0;
    for(i=0; i<FD_SETSIZE; ++i) {
	if(portmap[i]==s) {
	    portmap[i]=0;
	}
    }
}
