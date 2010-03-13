#include "header.h"

struct event serverSocketEvent;
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

    fcntl(0, F_SETFL, O_NONBLOCK);
    event_set(&serverSocketEvent, ss, EV_READ, ss_onconnect, &serverSocketEvent);
    event_add(&serverSocketEvent, NULL);
}

char buffer[1024];
char* errmsg[FD_SETSIZE];
int portmap[FD_SETSIZE];
char* writedebt[FD_SETSIZE];
int writedebtlen[FD_SETSIZE]={[0 ... FD_SETSIZE-1]=0};

void ss_onconnect(int ss, short event, void *arg) {
    struct sockaddr_in sa,da,ka;
    int len = sizeof sa;
    int s = accept(ss, (struct sockaddr*) &sa, &len);
    fcntl(s, F_SETFL, O_NONBLOCK);

    event_add((struct event*)arg, NULL);
    
    struct bufferevent *sBuf = bufferevent_new(s, s_ready, NULL, s_except, NULL);
    bufferevent_enable(sBuf, EV_READ|EV_WRITE);



    len = sizeof da;
    if (getsockopt(s, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr *)&da, &len) != 0) {
	evbuffer_add_printf(sBuf->output, "Unable to get destination address\n");
    }


    printf("%s:%d -> %s->%d\n", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), inet_ntoa(da.sin_addr), ntohs(da.sin_port));
    /*
    int cs = socket(PF_INET, SOCK_STREAM, 0);
    if (cs==-1) {
	evbuffer_add_printf(sBuf->output, "Unable to create client socket to connect to SOCKS server\n");
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
    */
}


void s_ready(struct bufferevent *s, void * arg) {
    ssize_t len = bufferevent_read(s, buffer, sizeof buffer);
    if (!len) {
	bufferevent_disable(s, EV_READ|EV_WRITE);
	bufferevent_free(s);
	return;
    }
    bufferevent_write(s, buffer, len);
}


void s_except(struct bufferevent *s, short what, void * arg) {
    bufferevent_disable(s, EV_READ|EV_WRITE);
    bufferevent_free(s);
}

