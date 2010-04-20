/* Header files list is just copied from various examples. There may be extras or omissions */
#include <stdio.h>
#include <sys/epoll.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>
#include <signal.h>		/* Only for backtracer */
//#include <linux/netfilter_ipv4.h> /* Just remove if not found, SO_ORIGINAL_DST = 80 */

#define MAXFD 4096		/* Not checked for overflow anywhere */

struct {
    int peerfd;
    char status;
    /*
       C - connected client
       c - connected client, write-ready

       S - pending connection to SOCKS5 server
       1 - got initial response
       2 - got auth methods list response 
       3 - got connection succeed response

       | - bidirectional connected peer (idle, can send both here and to peer)
       s - bidirectional connected peer (idle, can send here, not to peer)
       p - bidirectional connected peer (idle, can send to peer, not here)
       - - bidirectional connected peer (idle, cannot send (write is not ready))
       > - can only send (half-shutdown)
       ) - can only send (half-shutdown), but not write-ready
       < - can onty recv (half-shutdown)
       ( - can onty recv (half-shutdown), but peer is not write-ready
       . - closed
     */
} fdinfo[MAXFD] = { [0 ... MAXFD - 1] = {0, 0}};

char *argv0;

/* Unrelated to the main task */
void print_fdinfo()
{
    int i, m;
    for (i = 0; i < MAXFD; ++i) {
	if (fdinfo[i].status) {
	    m = i;
	}
    }

    fprintf(stderr, "fdinfo:");
    for (i = 0; i <= m; ++i) {
	if (fdinfo[i].status) {
	    fprintf(stderr, "%c", fdinfo[i].status);
	} else {
	    fprintf(stderr, " ");
	}
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "fdpeer:");
    for (i = 0; i <= m; ++i) {
	if (fdinfo[i].status) {
	    if (fdinfo[i].peerfd >= i) {
		fprintf(stderr, "%c", '0' + fdinfo[i].peerfd - i);
	    } else {
		fprintf(stderr, "%c", 'z' - i + fdinfo[i].peerfd);
	    }
	}
    }
    fprintf(stderr, "\n");
}

void print_trace()
{
    print_fdinfo();
    write(2, "*** BACKTRACE ***\n", 19);
    char buf[30];
    sprintf(buf, "%d", getpid());
    int q = fork();
    if (!q) {
	execlp("gdb", "gdb", "-batch", "-n", "-ex", "bt full", argv0, buf,
	       NULL);
    } else {
	wait(q);
    }
    write(2, "*** END OF BACKTRACE ***\n", 26);
}

void bad_signal()
{
    print_trace();
    exit(2);
}


int main(int argc, char *argv[])
{
    char *bind_ip;
    int bind_port;
    char *socks_ip;
    int socks_port;
    char *socks_username = NULL;
    char *socks_password = NULL;

    /* Setup the backtracer */
    {
	argv0 = argv[0];
	struct sigaction sa = { bad_signal };
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sa.sa_handler = &print_trace;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = &print_fdinfo;
	sigaction(SIGUSR2, &sa, NULL);
    }

    if (argc != 5 && argc != 7) {
	printf
	    ("Usage: socksredirect 0.0.0.0 1234 127.0.0.1 1080 [username password]\n"
	     "                     bind_ip port socks_ip socks_port\n");
	return 1;
    }

    /* Setup the command line arguments */
    bind_ip = argv[1];
    bind_port = atoi(argv[2]);
    socks_ip = argv[3];
    socks_port = atoi(argv[4]);
    if (argc >= 7) {
	socks_username = argv[5];
	socks_password = argv[6];
    }


    /* Open the server side socket */
    int ss = socket(PF_INET, SOCK_STREAM, 0);
    if (ss == -1) {
	perror("socket");
	exit(1);
    }

    int opt = 1;
    setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    fcntl(ss, F_SETFL, O_NONBLOCK);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = PF_INET;
    sa.sin_port = htons(bind_port);
    inet_aton(bind_ip, &sa.sin_addr);
    if (-1 == bind(ss, (struct sockaddr *) &sa, sizeof sa)) {
	close(ss);
	perror("bind");
	exit(1);
    }
    if (-1 == listen(ss, 0)) {
	close(ss);
	perror("listen");
	exit(1);
    }
    fcntl(0, F_SETFL, O_NONBLOCK);

    /* epoll setup */
    struct epoll_event ev, events[4];
    int kdpfd = epoll_create(4);
    if (kdpfd == -1) {
	perror("epoll_create");
	print_trace();
	exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN;
    ev.data.fd = ss;
    if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, ss, &ev) == -1) {
	perror("epoll_ctl: listen_sock");
	print_trace();
	exit(EXIT_FAILURE);
    }



    /* Main event loop */
    for (;;) {
	int nfds = epoll_wait(kdpfd, events, 4, -1);

	if (nfds == -1) {
	    if (errno == EAGAIN || errno == EINTR) {
		continue;
	    }
	    perror("epoll_pwait");
	    print_trace();
	    exit(EXIT_FAILURE);
	}


	int n;
	for (n = 0; n < nfds; ++n) {
	    if (events[n].data.fd == ss) {
		/* Accepting the client socket */
		struct sockaddr_in sa, da, ka;
		int len = sizeof sa;
		int client = accept(ss, (struct sockaddr *) &sa, &len);
		if (client < 0) {
		    perror("accept");
		    print_trace();
		    continue;
		}
		fcntl(client, F_SETFL, O_NONBLOCK);

		len = sizeof da;
#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif
		if (getsockopt
		    (client, SOL_IP, SO_ORIGINAL_DST,
		     (struct sockaddr *) &da, &len) != 0) {
		    write(client, "Unable to get destination address\n",
			  34);
		    close(client);
		    print_trace();
		    printf("%s:%d -> ???\n", inet_ntoa(sa.sin_addr),
			   ntohs(sa.sin_port));
		    continue;
		}

		printf("%s:%d -> %s->%d\n", inet_ntoa(sa.sin_addr),
		       ntohs(sa.sin_port), inet_ntoa(da.sin_addr),
		       ntohs(da.sin_port));

		ev.events = EPOLLIN;
		ev.data.fd = client;
		if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, client, &ev) < 0) {
		    fprintf(stderr, "epoll set insertion error\n");
		    print_trace();
		    write(client, "epoll set insertion error\n", 26);
		    close(client);
		    continue;
		}

		/* Now start connecting to SOCKS5 server */

		int sockssocket = socket(PF_INET, SOCK_STREAM, 0);
		if (sockssocket == -1) {
		    fprintf(stderr,
			    "Cannot create a socket to connect to the SOCKS5 server\n");
		    print_trace();
		    write(client,
			  "Cannot create a socket to connect to the SOCKS5 server\n",
			  55);
		    close(client);
		    continue;
		}
		fcntl(sockssocket, F_SETFL, O_NONBLOCK);
		memset(&ka, 0, sizeof ka);
		ka.sin_family = PF_INET;
		ka.sin_port = htons(socks_port);
		inet_aton(socks_ip, &ka.sin_addr);
		if (-1 == connect(sockssocket, (struct sockaddr *) &ka, sizeof ka)) {
		    if (errno != EWOULDBLOCK && errno != EINPROGRESS) {
			fprintf(stderr,
				"Cannot connect a socket to SOCKS5 address\n");
			print_trace();
			write(client,
			      "Cannot connect a socket to SOCKS5 address\n",
			      42);
			close(client);
			continue;
		    }
		}

		if (!socks_username) {
		    char socks_connect_request[3 + 4 + 4 + 2];	/* Noauth method offer + connect command + IP address + port */
                    sprintf(socks_connect_request, "\x5\x1\x0"       "\x5\x1\x0\x1"    "XXXX"       "XX" );
		    /*                                      |              |     |         |          |
							  no auth        connect |        IP address  |
		                                                                IPv4                 port	*/
		    memcpy(socks_connect_request+3+4   , &da.sin_addr,4);
		    memcpy(socks_connect_request+3+4+4 , &da.sin_port,2);
		    
		}

		/* Not we need to go through epoll_wait and wait when sockssocket will be ready for sending */


	    } else {
		//do_use_fd(events[n].data.fd);
	    }
	}
    }



    fprintf(stderr, "Probably abnormal termination\n");
    print_trace();
}
