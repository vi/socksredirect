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


