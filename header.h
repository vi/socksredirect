#include <stdio.h>
#include <sys/select.h> 
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>   
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <netinet/in.h>

typedef void (*handler)(int fd);
typedef handler handlers[FD_SETSIZE];

extern void(*cmds[256])();


void cmd_help_impl();
void cmd_help();
void cmd_quit();
void cmd_new();
void handle_stdin(int fd);
void handle_stdin_exception(int fd);
void cmds_init();
extern int maxfd;
extern fd_set rin, win, ein;
extern fd_set rout, wout, eout;
extern handlers rha, wha, eha;
extern int nfound;
void register_in(int fd, handler h);
void register_out(int fd, handler h);
void register_except(int fd, handler h);
int main(int argc, char* argv[]);
void init_server_socket();
void ss_onconnect(int ss);
void ss_except(int ss);
