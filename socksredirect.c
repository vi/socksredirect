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

#ifndef SPLICE_F_NONBLOCK
    #define SPLICE_F_NONBLOCK	2
#endif

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

       P - this is a pipe, peerfd should be some socket
     */   
    struct sockaddr_in da;
    int pipe; // recv'ed data is spliced to this pipe
    int pipeout; // output pipe end
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

char buf[64];

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


		/* Set up events and pipes and fdinfo*/
		
		int cpipefd[2], spipefd[2];
		int cret, sret;
		cret=pipe2(cpipefd, O_NONBLOCK);
		sret=pipe2(spipefd, O_NONBLOCK);
		if(cret<0 || sret<0) {
		    perror("pipe2");
		    close(client);
		    close(sockssocket);
		    if(cret>=0) { close(cpipefd[0]); close(cpipefd[1]);  }
		    if(sret>=0) { close(spipefd[0]); close(spipefd[1]);  }
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
		fdinfo[sockssocket].peerfd = client;
		fdinfo[sockssocket].status='S';
		fdinfo[sockssocket].da=da;
		fdinfo[sockssocket].writeready=0;
		fdinfo[sockssocket].readready=0;
		fdinfo[client].peerfd = sockssocket;
		fdinfo[client].status='C';
		fdinfo[client].da=da;
		fdinfo[client].writeready=0;
		fdinfo[client].readready=0;

		fdinfo[cpipefd[0]].status='P';
		fdinfo[cpipefd[0]].peerfd = sockssocket;
		fdinfo[client].pipe=cpipefd[1];
		fdinfo[client].pipeout=cpipefd[0];

		fdinfo[spipefd[0]].status='P';
		fdinfo[spipefd[0]].peerfd = client;
		fdinfo[sockssocket].pipe=spipefd[1];
		fdinfo[sockssocket].pipeout=spipefd[0];
		


	    } else {
		int fd=events[n].data.fd;
		int peerfd=fdinfo[fd].peerfd;
		int status = fdinfo[fd].status;
		int peerstatus = fdinfo[peerfd].status;
		char writeready=0;
		char readready=0;

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
			    fprintf(stderr, "This branch should not happed\n");
			    print_trace();
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
			    close(fdinfo[fd].pipe);
			    close(fdinfo[fd].pipeout);
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

			    /* SOCKS5 connection successed. Setting up piping. */

			    /* peerstatus is 'C' */
			    status='|';
			    peerstatus='|';

			    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET ;
			    ev.data.fd = peerfd;
			    if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, peerfd, &ev) < 0) {
				write(peerfd, "epoll set insertion error\n", 26);
				status='.';
				continue;
			    }
			    
			    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET ;
			    ev.data.fd = fdinfo[fd].pipeout;
			    if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, fdinfo[fd].pipeout, &ev) < 0) {
				write(peerfd, "epoll set insertion error\n", 26);
				status='.';
				continue;
			    }
			    
			    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET ;
			    ev.data.fd = fdinfo[peerfd].pipeout;
			    if (epoll_ctl(kdpfd, EPOLL_CTL_ADD, fdinfo[peerfd].pipeout, &ev) < 0) {
				write(peerfd, "epoll set insertion error\n", 26);
				status='.';
				continue;
			    }
			    

			    break;

			case '|':
			case 'r':
			    {
				int q,w;
				for(;;) {
				    q=splice(fd, NULL, fdinfo[fd].pipe, NULL, 65536, SPLICE_F_NONBLOCK);
				    if(q<=0) {
					fdinfo[fd].readready=0;
					break;
				    }
				    fprintf(stderr, "Spliced %d bytes to pipe\n", q);
				    w=splice(fdinfo[fd].pipeout, NULL, peerfd, NULL, q, SPLICE_F_NONBLOCK);
				    if(w<q) {
					fdinfo[peerfd].writeready=0;
					break;
				    }
				    fprintf(stderr, "    Spliced %d bytes from pipe\n", w);
				}
			    }
			    break;
			case 'P':
			    if(fdinfo[peerfd].writeready) {
				int q;
				q=splice(fd, NULL, peerfd, NULL, 65536, SPLICE_F_NONBLOCK);
				fprintf(stderr, "Spliced %d bytes of debt\n", q);
			    }
		    }
		}
		if(writeready) {
		    if(status=='|' || status=='s') {
			int q=1,w;
			for(;;) {
			    w=splice(fdinfo[fd].pipeout, NULL, fd, NULL, q, SPLICE_F_NONBLOCK);
			    if(w<q) {
				fdinfo[fd].writeready=0;
				writeready=0;
				break;
			    }
			    fprintf(stderr, "Spliced %d bytes from pipe\n", w);

			    q=splice(peerfd, NULL, fdinfo[fd].pipe, NULL, 65536, SPLICE_F_NONBLOCK);
			    if(q<=0) {
				fdinfo[peerfd].readready=0;
				break;
			    }
			    fprintf(stderr, "    Spliced %d bytes to pipe\n", q);
			}
		    }
		}
		if (status=='.' || !status) {
		    peerstatus=0;
		    status=0;
		    close(fd);
		    close(peerfd);
		    close(fdinfo[fd].pipe);
		    close(fdinfo[fd].pipeout);
		    close(fdinfo[peerfd].pipe);
		    close(fdinfo[peerfd].pipeout);
		    fdinfo[fdinfo[fd].pipeout].status=0;
		    fdinfo[fdinfo[peerfd].pipeout].status=0;
		}
		fdinfo[fd].status=status;
		fdinfo[peerfd].status=peerstatus;
	    }
	}
    }



    fprintf(stderr, "Probably abnormal termination\n");
    print_trace();
}
