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

#include <event.h>

#ifndef SO_ORIGINAL_DST
    #define SO_ORIGINAL_DST 80
#endif

extern void(*cmds[256])();


