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
char* writedebt[FD_SETSIZE];
int writedebtlen[FD_SETSIZE]={[0 ... FD_SETSIZE-1]=0};

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
    writedebtlen[s]=0;
    writedebtlen[cs]=0;

}

void s_err(int s, char* msg) {
    unregister(s);
    unportmap(s);
    ssize_t wlen = write(s, msg, strlen(msg));
    if (wlen==-1) {
	if(errno==EWOULDBLOCK) {
	    errmsg[s]=msg;
	    register_out(s, s_err2);
	    shutdown(s, SHUT_RD);
	    return;
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
    int cs = portmap[s];
    ssize_t len = read(s, buffer, sizeof buffer);
    if(len==-1) {
	s_except(s);
	return;
    }
    if (!len) {
	shutdown(s, SHUT_RD);
        if (cs) {
	    shutdown(cs, SHUT_WR);
	}
	portmap[s]=0;
	if (!portmap[cs]) {
	    close(s);
	    unregister(s);
	    close(cs);
	    unregister(cs);
	}
	return;
    }
    if(cs) {
	ssize_t wlen = write(cs, buffer, len);
	if (wlen==-1) {
	    if(errno==EWOULDBLOCK) {
		debt(cs, buffer, len);
		return;
	    } else {
		s_except(cs);
		return;
	    }
	}
	if (wlen<len) {
	    debt(cs, buffer+wlen, len-wlen);
	    return;
	}
	register_in(s, s_ready);
    } else {
	fprintf(stderr, "Nowhere to route data from socket %d\n", s);
	kill(s);
    }
}

void debt(int s, char* buf, int len) {
    char *buf2 = (char*)malloc(len);
    memcpy(buf2, buf, len);
    writedebt[s]=buf2;
    writedebtlen[s]=len;
    register_out(s, s_debt);
}

void s_debt(int s) {
    char* buf = writedebt[s];
    int len = writedebtlen[s];
    // assert(len>0)
    writedebtlen[s]=0;

    int wlen;
    wlen = write(s, buf, len);
    if (wlen==-1 || !wlen) {
	s_except(s);
	return;
    }
    if(wlen<len) {
	debt(s, buf+wlen, len-wlen);
    } else {
	int rs = portmap[s];
	if(rs)	{
	    register_in(rs, s_ready);
	} else {
	    fprintf(stderr, "Missing portmap after debt write\n");
	}
    }
    free(buf);

}

void killsock(int s) {
    if(writedebtlen[s]) {
	free(writedebt[s]);
    }
    unregister(s);
    unportmap(s);
    close(s);
}

void s_except(int s) {
    int cs = portmap[s];
    killsock(s);
    if(cs) {
	killsock(cs);
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

int ismapped(int s) {
    int i;
    for(i=0; i<FD_SETSIZE; ++i) {
	if(portmap[i]==s) {
	    return 1;
	}
    }
    return 0;
}
