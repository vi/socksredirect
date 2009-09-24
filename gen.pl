#!/usr/bin/perl -w
#
# gen.pl:
#
# Copyright (c) 2009 Vitaly "_Vi" Shukela. Some rights reserved.
# 
#

my $rcsid = ''; $rcsid .= '$Id:$';

use strict;

`cp header_common.h header.h`;

open G, ">>header.h";
while(<*.c>) {
    open F, "<", $_;

    my $vars = 0;

    while(<F>) {
	m!//\s*vars! and $vars=1 and next;
	m!//\s*end vars! and $vars=0;
	if($vars) {
	    if(m!(\w+)\s+(.*);!) {
		my $t=$1;
		my $v=$2;
		my @a = split ',\s*', $v;
		my @b = map { s/\s*=.*//; $_ } @a;
		print G "extern $t ".(join ', ', @b).";\n";
	    }
	} else {
	    if(m!(void|char\*?|int)\s+(\w+)\s*\((.*)\)\s*{!) {
		print G "$1 $2($3);\n";
	    }
	}
    }
    close F;
}

open F, "cmds.c";
open H, ">cmds_init.c";
open R, ">cmd_help.c";

print H '#include "header.h"', "\n";
print R '#include "header.h"', "\n";
print H 'void cmds_init() {', "\n";
print R 'void cmd_help_impl() {', "\n";

my $keys;
my $help;
while(<F>) {
    $keys = $1 if m!//=\s*(\S+)!;
    $help = $1 if m!//\?\s*(.+)!;
    if(m!void (cmd_\w+)!) {
	my $c = $1;
	my @k = split '', $keys;
	foreach my $i (@k) {
	    print H "\tcmds['$i']=$c;\n" if $keys;
	}
	print R "\tfprintf(stderr, \"'".(join ',',@k)."' - ".($help?$help:$c)."  \");\n";
	$keys=$help=undef;
    }
}

print H '}', "\n";
print R "\tfprintf(stderr, \"\\n\");\n}\n";
close H;

close G;
