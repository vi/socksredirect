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
    char writeready;
    char readready;
    char status;
    /*
       States:
       C - connected client
       c - connected client, but has already signed off sending anything (cannot recv from him)
       i - connected client, but already cannot send to him

       S - pending connection to SOCKS5 server
       0 - sent response
       1 - got auth method choice response

       | - bidirectional connected peer
       s - we can only send (half-shutdown)
       r - we can only recv (half-shutdown)
       . - closed
     */   
    struct sockaddr_in da;
} fdinfo[MAXFD] = { [0 ... MAXFD - 1] = {0, 0, 0}};

char *argv0;                     // Close if cannot both send and recv

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
	} else {
	    fprintf(stderr, " ");
	}  
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "fdread:");
    for (i = 0; i <= m; ++i) {
	if (fdinfo[i].status) {
	    fprintf(stderr, "%c",  "NWRB"[fdinfo[i].writeready+2*fdinfo[i].readready]);
	} else {
	    fprintf(stderr, " ");
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

char buf[65536];

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

		printf("%s:%d -> %s:%d\n", inet_ntoa(sa.sin_addr),
		       ntohs(sa.sin_port), inet_ntoa(da.sin_addr),
		       ntohs(da.sin_port));


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


		/* Set up events */

		ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET ;
		ev.data.fd = client;
		if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, client, &ev) < 0) {
		    fprintf(stderr, "epoll set insertion error\n");
		    print_trace();
		    write(client, "epoll set insertion error\n", 26);
		    close(client);
		    continue;
		}

		ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
		ev.data.fd = sockssocket;
		if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, sockssocket, &ev) < 0) {
		    fprintf(stderr, "epoll peer set insertion error\n");
		    print_trace();
		    write(client, "epoll peer set insertion error\n", 31);
		    close(client);
		    continue;
		}
		


		/* Not we need to go through epoll_wait and wait when sockssocket will be ready for sending */
		fdinfo[client].peerfd = sockssocket;
		fdinfo[sockssocket].peerfd = client;
		fdinfo[client].status='C';
		fdinfo[sockssocket].status='S';
		fdinfo[sockssocket].da=da;
		fdinfo[client].da=da;
		fdinfo[client].writeready=0;
		fdinfo[client].readready=0;
		fdinfo[sockssocket].writeready=0;
		fdinfo[sockssocket].readready=0;


	    } else {
		int fd=events[n].data.fd;
		int peerfd=fdinfo[fd].peerfd;
		int status = fdinfo[fd].status;
		int peerstatus = fdinfo[peerfd].status;
		char writeready=0;
		char readready=0;

		if(fdinfo[peerfd].peerfd != fd) { fprintf(stderr, "Hmmm...\n"); }

		if(events[n].events&EPOLLIN) {
		    fdinfo[fd].readready = 1;
		    readready=1;
		    //fprintf(stderr,"%d IN\n", fd);
		}
		if(events[n].events&EPOLLOUT) {
		    fdinfo[fd].writeready = 1;
		    writeready=1;
		    //fprintf(stderr,"%d OUT\n", fd);
		}           
		if(events[n].events&EPOLLRDHUP) {
		    //fprintf(stderr,"%d RDHUP\n", fd);
		    fdinfo[fd].readready = 0;
		    switch(status) {
			case 'C': 
			    status='c'; 
			    shutdown(fd, SHUT_RD);
			    break;
			case 'S': 
			case '0':
			case '1': 
			case '2': 
			    write(peerfd, "Premature EOF from SOCKS5 server\n", 77-44);
			    status='.';  
			    break;
			case '|': 
			    status='s'; 
			    peerstatus='r';
			    shutdown(fd, SHUT_RD);
			    shutdown(peerfd, SHUT_WR);
			    break;
			case 'r':
			    status='.'; // Close if cannot both send and recv
			    break; 
		    }
		}
		if(events[n].events&(EPOLLERR|EPOLLHUP) ) {
		    //fprintf(stderr,"%d HUP\n", fd);
		    status='.';
		}

		if(status=='S' && writeready) {
		    if (!socks_username) {
			char socks_connect_request[3 + 4 + 4 + 2];	/* Noauth method offer + connect command + IP address + port */
			memcpy(socks_connect_request,  "\x5\x1\x0"       "\x5\x1\x0\x1"    "XXXX"       "XX" , 3+4+4+2);
			/*                                      |              |     |         |          |
							      no auth        connect |        IP address  |
										    IPv4                 port	*/
			memcpy(socks_connect_request+3+4   , &fdinfo[fd].da.sin_addr,4);
			memcpy(socks_connect_request+3+4+4 , &fdinfo[fd].da.sin_port,2);

			write(fd, socks_connect_request, 3+4+4+2);				
			status='0';
		    }
		}
		if(readready) {
		    int nn;
		    switch(status) {
			case 'S':
			case '0':
                            nn = read(fd, buf, 2);
			    //fprintf(stderr, "SOCKS5 phase 0 reply: %02x%02x\n", buf[0], buf[1]);
			    if(nn!=2) {
				write(peerfd, "Not exactly 2 bytes is received from SOCKS5 server. This situation is not handeled.\n", 132-48);
				status='.';
				break;
			    }
			    if(buf[0]!=5 || (buf[1]!=0 && buf[1]!=255)) {
				write(peerfd, "Not SOCKS5 reply from SOCKS5 server\n", 84-48);
				status='.';
				break;
			    }
			    if(buf[1]==255) {
				write(peerfd, "Authentication requred on SOCKS5 server\n", 88-48);
				status='.';
				break;
			    }
			    status='1';
			    break;
			case '1':
                            nn = read(fd, buf, 10);
			    //fprintf(stderr, "SOCKS5 phase 1 reply: %02x%02x%02x%02x...\n", buf[0], buf[1], buf[2], buf[3]);
			    if(nn<10) {
				write(peerfd, "Less then 11 bytes is received from SOCKS5 server. This situation is not handeled.\n", 131-48);
				status='.';
				print_trace();
				break;
			    }
			    if(buf[0]!=5) {
				write(peerfd, "Not SOCKS5 reply from SOCKS5 server\n", 84-48);
				status='.';
				break;
			    }
			    if(buf[1]!=0) {
				switch(buf[1]) {
				    case 1: write(peerfd, "general SOCKS server failure\n", 89-60); break;
				    case 2: write(peerfd, "connection not allowed by ruleset\n",  94-60); break; 
				    case 3: write(peerfd, "Network unreachable\n", 80-60); break; 
				    case 4: write(peerfd, "Host unreachable\n", 77-60); break; 
				    case 5: write(peerfd, "Connection refused\n", 79-60); break; 
				    case 6: write(peerfd, "TTL expired\n", 72-60); break; 
				    case 7: write(peerfd, "Command not supported\n", 82-60); break; 
				    case 8: write(peerfd, "Address type not supported\n", 87-60); break; 
				    default: write(peerfd,"Unknown error at SOCKS5 server\n", 91-60); break; 
				}
				status='.';
			    }
			    if(buf[3]!=1) {
				write(peerfd, "Not an IPv4 address in SOCKS5 reply\n", 84-48);
				status='.';
				break;
			    }
			    if(peerstatus=='c') {
				shutdown(fd, SHUT_WR);
				shutdown(peerfd, SHUT_RD);
				status='r';
				peerstatus='s';
			    } else {
				/* peerstatus is 'C' */
                                status='|';
				peerstatus='|';
			    }
			    break;

			case '|':
			case 'r':
			    if(fdinfo[peerfd].writeready) {
				int q;
				nn = read(fd, buf, sizeof buf);
				q=write(peerfd, buf, nn);
				if(q!=nn) {
				    fprintf(stderr, "Error, wrote only %s bytes instead of %d\n", q, nn);
				    status='.';
				    break;
				}
				fdinfo[peerfd].writeready=0;
				fdinfo[fd].readready=0;
			    }
			    break;
		    }
		}
		if (status=='.') {
		    peerstatus='.';
		    close(fd);
		    close(peerfd);
		}
		fdinfo[fd].status=status;
		fdinfo[peerfd].status=peerstatus;
	    }
	}
    }



    fprintf(stderr, "Probably abnormal termination\n");
    print_trace();
}
