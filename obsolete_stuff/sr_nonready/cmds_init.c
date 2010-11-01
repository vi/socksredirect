#include "header.h"
void cmds_init() {
	cmds['h']=cmd_help;
	cmds['?']=cmd_help;
	cmds['q']=cmd_quit;
	cmds['n']=cmd_new;
}
