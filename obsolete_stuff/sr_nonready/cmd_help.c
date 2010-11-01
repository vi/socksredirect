#include "header.h"
void cmd_help_impl() {
	fprintf(stderr, "'h,?' - Help  ");
	fprintf(stderr, "'q' - Quit  ");
	fprintf(stderr, "'n' - New  ");
	fprintf(stderr, "\n");
}
