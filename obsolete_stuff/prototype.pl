#!/usr/bin/perl -w
#
# socksredirect: 
#   iptables -t nat -A OUTPUT -p tcp ! -d localnetwork -j REDIERCT --to-ports 1234
#   socksredirect 1234 address_of_socks_server [user passoword]
#  
#   Uses /proc/net/nf_conntrack to restore destination address and port
#
# Copyright (c) 2009 Vitaly "_Vi" Shukela. Some rights reserved.
# 
#

my $rcsid = ''; $rcsid .= '$Id:$';

use strict;
use Socket qw(IPPROTO_TCP AF_INET SOCK_STREAM SOL_SOCKET SO_REUSEADDR SO_ERROR AF_INET SOCK_STREAM inet_aton sockaddr_in inet_ntoa);  
use Fcntl qw(F_GETFL F_SETFL O_NONBLOCK);
use POSIX qw(EINPROGRESS);

use constant {
    READLENGTH=>1024
};


my $addresstolisten=shift;
my $porttolisten=shift;
my $addresstoconnect=shift;
my $porttoconnect=shift;
die("Usage: socksredirect address_to_listen port_to_listen address_of_socks_server [user password [oneround [maxpend [maxconn]]]]\nPreparation: iptables -t nat -A OUTPUT -p tcp -d destination_to_be_socksified -j REDIERCT --to-ports port_to_listen\n") unless $porttoconnect;
my $user=shift;
my $password=shift;
my $mode=shift||"";
my $maxpend=shift||20;
my $maxconn=shift||40;

sub SIGPIPE {
    # just ignore it. The writer will handle.
    print STDERR "SIGPIPE!\n";
    $SIG{PIPE}=\&SIGPIPE;
}
$SIG{PIPE}=\&SIGPIPE;


socket(SS, AF_INET, SOCK_STREAM, 0)  or die "socket: $!";

setsockopt(SS, SOL_SOCKET, SO_REUSEADDR, pack("l", 1)) or die "setsockopt: $!";

fcntl(SS, F_SETFL, O_NONBLOCK);

bind(SS, sockaddr_in($porttolisten, inet_aton($addresstolisten))) or die "bind: $!";

listen(SS,0) or die "listen: $!";

my %portmap;
my %socksinfo;
my %pendingconnect;
my %writedebt;

my $rin = my $win = my $ein = '';
vec($rin,fileno(SS),1) = 1;
vec($rin,0,1) = 1;

my $counter=0;
my %ager;

sub establishsocks_init($);
sub establishsocks_oneround($);

sub writer($$$$);
sub killsock($);
sub unregistersock($);

my $rout = $rin;
my $wout = $win;
sub dumpsel();

my $buffer;
my $readsock;
my $writesock;
my $readret;
my $writeret;
my $offset;

my $nfound;

my $i;

sub killall() {
    my %union = map(($_=>0), (keys %portmap, keys %socksinfo, keys %pendingconnect, keys %writedebt));
    map {open Q, "<&=", $_; killsock(*Q);} keys %union;
}

for(;;){
    $nfound = select($rout=$rin, $wout=$win, undef, undef);

    if($nfound==-1) {
	print STDERR "select: $!\n";
	killall();
	$rin = $win = $ein = '';
	vec($rin,fileno(SS),1) = 1;
	vec($rin,0,1) = 1;
	next;
    }

    if(vec($rout,0,1)) {
	--$nfound;
	$_ = <STDIN>;
	if(/^k/) {
	    killall();
	}
	if(/^s/) {
	    print STDERR 
		" m:",scalar keys %portmap,
		" s:",scalar keys %socksinfo,
		" p:",scalar keys %pendingconnect,
		" d:",scalar keys %writedebt,
		"\n";
	}
	if(/^q/){
	    last;
	}
	if(/^f/) {
	    dumpsel;
	}
	if (/^c/) {
	    sub checkhash($$) {
		my ($h, $n) = @_;
		foreach my $i (keys %$h) {
		    open Q, ">&=", $i;
		    unless(defined fileno(Q)) {
			print STDERR "$i is invalid in $n";
			unregistersock($i);
		    }
		}
	    }
	    checkhash(\%portmap, "portmap");
	    checkhash(\%socksinfo, "socksinfo");
	    checkhash(\%writedebt, "writedebt");
	    checkhash(\%pendingconnect, "pendconnecting");
	    foreach my $i (keys %portmap) {
		my $writesock     = $portmap{$i};
		my $readsock      = $portmap{fileno($writesock)};
		print STDERR "$i -> ".fileno($writesock)." -> ".fileno($readsock)." in portmap\n" unless $i==fileno($readsock);
	    }
	    foreach my $i (keys %socksinfo) {
		my $si = $socksinfo{$i};
		unless(defined fileno($si->{peer})) {
		    print STDERR "peer is invalid in socksinfo $i";
		    if(defined fileno($si->{so})) {
			close $si->{so};
		    }
		    unregistersock($i);
		}
	    }
	}
	if(/^l/) {
	    print STDERR " maxconn=$maxconn  maxpend=$maxpend\n";
	}
	if(/^e(.*)/) {
	    eval $1;
	}
	if(/\?/) {
	    print STDERR " Stats Quit Fds Kill fsCk Limits Eval\n";
	}
    }

    if(vec($rout,fileno(SS),1)){
	--$nfound;
	while((scalar keys %pendingconnect)+(scalar keys %socksinfo)>$maxpend) {
	    print STDERR "Pending limit reached:".(scalar keys %pendingconnect)."+".(scalar keys %socksinfo)." > $maxpend\n";
	    my %union = map(($_=>0), (keys %portmap, keys %socksinfo));
	    my $victim = [ sort { $ager{$a} <=> $ager{$b} } keys %union ]->[0];  # oldest of establishing sockets
	    print STDERR "Asertion failed: victim not found\n" unless $victim;
	    if(exists $socksinfo{$victim}) {
		my $si = $socksinfo{$victim};
		my $peer = $si->{peer};
		if(defined fileno($peer)) {
		    print $peer, "You inactive connection was sacrificed for the newer one\n";
		    killsock($peer);	  
		} else {
		    print STDERR "    peer is invalid for $victim\n";
		}
	    }
	    open Q, ">&=", $victim;
	    unless(defined fileno(Q)) {
		print STDERR  "    $victim is invalid\n";  
		unregistersock($victim);
	    } else {
		killsock(*Q);
	    }
	}
	my $s;
	my $paddr = accept($s, SS);
	$ager{fileno($s)}=++$counter;
	if ($paddr) {
	    fcntl($s, F_SETFL, O_NONBLOCK);
	    my($port,$iaddr) = sockaddr_in($paddr);

	    my $srcip = inet_ntoa($iaddr);
	    my $srcport = $port;

	    print STDOUT "$srcip:$port -> ";

	    my $destip;
	    my $destport;

	    open F, "</proc/net/nf_conntrack" or print STDERR "Cannot open /proc/net/nf_conntrack\n";
	    while(<F>) {
		/src=$srcip dst=([0-9.]+) sport=$srcport dport=(\d+)/ and $destip=$1 and $destport=$2;
	    }
	    close F;

	    unless($destip and $destport) {
		print STDOUT "NULL?\n";
		print $s "Sorry, this port is intended to be connected using conntrack\n";
		close $s;
	    } else {
		print STDOUT "$destip:$destport\n";

		my $so;
		socket($so, AF_INET, SOCK_STREAM, 0) or $so=undef;
		$ager{fileno($so)}=++$counter;
		if($so) {
		    fcntl($so, F_SETFL, O_NONBLOCK);
		    select $so; $|=1;
		    connect($so, sockaddr_in($porttoconnect, inet_aton($addresstoconnect)));

		    my $si = { 
			soaddr=>$addresstoconnect, 
			soport=>$porttoconnect, 
			souser=>$user, 
			sopassword=>$password, 
			srcip=>$srcip,
			srcport=>$srcport,
			destip=>$destip, 
			destport=>$destport,
			peer=>$s,
			so=>$so,
			state=>($mode eq "oneround")?\&establishsocks_init_oneround:\&establishsocks_init,
			errortext=>"",
			bindip=>"",
			bindport=>"",
		    };
		    
		    $pendingconnect{fileno($so)}=$si;
		    $socksinfo{fileno($so)}=$si;
		    vec($win,fileno($so),1)=1;
		}	    
	    }
        }
    }

    foreach $i (keys %pendingconnect) {
	if(vec($wout,$i,1)) {
	    --$nfound;  
	    vec($win,$i,1)=0;
	    $ager{$i}=++$counter;
	    delete $pendingconnect{$i};
	    my $si = $socksinfo{$i};
	    my $s = $si->{peer};
	    my $so = $si->{so};

	    my $soerr = unpack("I", getsockopt($so, SOL_SOCKET, SO_ERROR));
	    if($soerr==0){
		$si->{state}($si);  # Call establishsocks_init
		vec($rin, fileno($so), 1)=1;

		if($si->{oneround}) {
		    # Setup sending end to enable early request transmission
		    vec($rin, fileno($s), 1)=1;
		    $portmap{fileno($s)}=$so;
		    $portmap{fileno($so)}=$s;
		}
		# wait for reply from SOCKS server, serving other requests

	    } else {
		print $s "Error: Unable to establish connection to SOCSK5 $addresstoconnect:$porttoconnect: $soerr\n";
		print STDERR "Unable to establish connection to SOCSK5 $addresstoconnect:$porttoconnect: $soerr\n";
		killsock($s);
		killsock($so);
	    }
	}	
    }
    
    # look for not yet negotiated SOCKS connections
    foreach $i (keys %socksinfo) {
	if(vec($rout,$i,1)) {
	    --$nfound;
	    $ager{$i}=++$counter;
	    my $si = $socksinfo{$i};

	    # Continue negotiation with SOCKS5
	    $si->{state}($si); # Next move
		
	    my $peer = $si->{peer};
	    my $so = $si->{so};
		
	    if($si->{errortext}) {
		print STDOUT "    $si->{srcip}:$si->{srcport} -> $si->{destip}:$si->{destport} ($si->{errortext})\n";  
		print $peer "Error: $si->{errortext}\n";
		killsock($so);
		killsock($peer);
		next;
	    }

	    unless(defined $si->{state}) { 
		# success, rewiring it to pormapper
		while((scalar keys %portmap)>$maxconn*2) {
		    print STDERR "Connections limit reached\n";
		    my $victim = [ sort { $ager{$a} <=> $ager{$b} } keys %portmap ]->[0];
		    my $writesock     = $portmap{$victim};
		    my $readsock      = $portmap{fileno($writesock)};
		    if(defined fileno($writesock)) {
			killsock($writesock);
			if(defined fileno($readsock)) {
			    killsock($readsock);
			} else {
			    unregistersock(fileno($writesock));
			}
		    } else {
			unregistersock($victim);
		    }
		}
		print STDOUT "    $si->{srcip}:$si->{srcport} -> $si->{destip}:$si->{destport} ($si->{bindip}:$si->{bindport})\n";  
		$portmap{fileno($peer)}=$so;
		$portmap{fileno($so)}=$peer;
		vec($rin, fileno($peer), 1)=1; # Begin receiving data from user
		vec($rout, fileno($so), 1)=0; # Do not read data from SOCKS server without selecting it
		delete $socksinfo{$i}; # Stop SOCKS negotiation phase
		next;
	    }  

	    # Next phase of SOCKS connection
	    # Server other things while waiting for it
	}
    }

    # Looks for portmappers (already negotiated with SOCKS server)
    foreach $i (keys %portmap) {
	if(vec($rout,$i,1)) {
	    $ager{$i}=++$counter;
	    next if exists $socksinfo{$i}; # avoid sending to clients negotiation requests from SOCKS server (oneround mode)
	    --$nfound;
	    my $writesock     = $portmap{$i};
	    my $readsock      = $portmap{fileno($writesock)};
	    unless (fileno($readsock)==$i) {
		print STDERR "Assertion failed: portmap is not paired?\n";
	    }
	    $readret = sysread $readsock, $buffer, READLENGTH;
	    writer($readsock, $writesock, $buffer, $readret);
	}
    }

    foreach $i (keys %writedebt) {
	if(vec($wout,$i,1)) {
	    $ager{$i}=++$counter;
	    --$nfound;
	    my $buffer = $writedebt{$i};
	    my $readsock     = $portmap{$i};
	    my $writesock    = $portmap{fileno($readsock)};
	    delete $writedebt{$i};
	    vec($rin,fileno($readsock),1)=1;
	    vec($win,fileno($writesock),1)=0;
	    print STDERR "writedebt length ".length $buffer."\n";
	    writer($readsock, $writesock, $buffer, length $buffer);
	}
    }

    print STDERR "Assertion failed: nfound=$nfound after processing\n" if $nfound;
}

sub shutdowndir($$);

sub writer($$$$) {
    my ($readsock, $writesock, $buffer, $count) = @_; 
    $offset=0;
    if($count) {
	while ($count) {
	    $writeret = syswrite $writesock, $buffer, $count, $offset;
	    unless($writeret) {
		if ($! == EINPROGRESS) {
		    $writedebt{fileno($writesock)} = substr $buffer, $offset;
		    vec($rin,fileno($readsock),1)=0;
		    vec($win,fileno($writesock),1)=1;
		} else {
		    # Shutdown this direction
		    shutdowndir($readsock, $writesock);
		}
		last;
	    }
	    $count-=$writeret;
	    $offset+=$writeret;
	}
	$ager{fileno($writesock)}=++$counter;
    } else {
	# Shutdown this direction
	shutdowndir($readsock, $writesock);
    }
}

sub shutdowndir($$) {
    my ($readsock, $writesock) = @_; 
    vec($rin,fileno($readsock),1)=0;
    shutdown($readsock, 0);
    shutdown($writesock, 1);
    unless (vec($rin,fileno($writesock),1)) {
	# close filehandles
	delete $portmap{fileno($writesock)};
	delete $portmap{fileno($readsock)};
	close $readsock;
	close $writesock;
    }
}
sub unregistersock($) {
    my $s = shift;
    vec($rin, $s, 1)=0;
    --$nfound if vec($rout, $s, 1);
    vec($rout, $s, 1)=0;
    vec($win, $s, 1)=0;
    --$nfound if vec($wout, $s, 1);
    vec($wout, $s, 1)=0;
    delete $portmap{$s};
    delete $writedebt{$s};
    delete $socksinfo{$s};
    delete $pendingconnect{$s};
}
sub killsock($) {
    my $s = shift;
    if(defined fileno($s)) {
	unregistersock(fileno($s));
        close $s;
    } else {
	print STDERR "Attempting to kill closed socket\n";
    }
}

sub dumpsel() {
    print STDERR "rin=(";
    for(my $i=0; $i<100; ++$i) {
	(print STDERR $i ," ") if vec($rin,$i,1)==1;
    }
    print STDERR ") win=(";
    for(my $i=0; $i<100; ++$i) {
	(print STDERR $i ," ") if vec($win,$i,1)==1;
    }
    print STDERR ")   rout=(";
    for(my $i=0; $i<100; ++$i) {
	(print STDERR $i ," ") if vec($rout,$i,1)==1;
    }
    print STDERR ") wout=(";
    for(my $i=0; $i<100; ++$i) {
	(print STDERR $i ," ") if vec($wout,$i,1)==1;
    }
    print STDERR ")\n";
}


sub es_noauth($);
sub es_userauth($);
sub es_userauth_recv($);
sub es_readytosend($);
sub es_finalizing($);

sub establishsocks_init($) {
    my $self = shift;
    my $so = $self->{so};
    
    $self->{oneround}=0;
    unless ($self->{souser}) {
	print $so pack("CCC", 5, 1, 0);
        $self->{state} = \&es_noauth;
    } else {
	print $so pack("CCC", 5, 1, 2);
	$self->{state} = \&es_userauth;
    }
}

sub es_noauth($) {
    my $self = shift;
    my $so = $self->{so};
    my $buf;

    sysread $so, $buf, 2;
    my ($ver, $method) = unpack "CC", $buf;
    $ver = -1 unless defined $ver;
    unless($ver==5) {
	if($ver==-1){
	    $self->{errortext} = "Unable to connect the SOCKS5 server $self->{soaddr}:$self->{soport}\n";
	}else{
	    $self->{errortext} = "socks version $ver? (should be 5 - SOCKS5)";
	}
	return;
    }
    $method = "EOF" unless defined $ver;
    unless($method==0) {
	$self->{errortext} = "auth method $method? (should be 0 - noauth)";
	return;
    }
    return es_readytosend($self);
}

sub es_userauth($) {
    my $self = shift;
    my $so = $self->{so};
    my $buf;    
    my $user = $self->{souser};
    my $password = $self->{sopassword};


    sysread $so, $buf, 2;
    my ($ver, $method) = unpack "CC", $buf;
    $ver = -1 unless defined $ver;
    unless($ver==5) {
	$self->{errortext} = "socks version $ver? (should be 5)";
	return;
    }
    $method = -1 unless defined $ver;
    unless($method==2) {
	$self->{errortext} = "auth method $method? (should be 2 - username/password)";
	return;
    }
    
    unless($self->{oneround}) {
	print $so pack("CC", 1, length($user)) . $user . pack("C", length($password)) . $password;
    }

    $self->{state} = \&es_userauth_recv;
}
 
sub es_userauth_recv($) {
    my $self = shift;
    my $so = $self->{so};
    my $buf;

    sysread $so, $buf, 2;
    my ($authver, $status) = unpack "CC", $buf;
    $authver = -1 unless defined $authver;
    unless($authver==1) {
	$self->{errortext} = "auth version $authver? (should be 1)";
	return;
    }
    $status = -1 unless defined $status;
    unless($status==0) {
	$self->{errortext} = "SOCKS5 authentication failed";
	return;
    }
    return es_readytosend($self);
}         

sub es_readytosend($) {
    my $self = shift;
    my $so = $self->{so};

=cut
        +----+-----+-------+------+----------+----------+
        |VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
        +----+-----+-------+------+----------+----------+
        | 1  |  1  | X'00' |  1   | Variable |    2     |
        +----+-----+-------+------+----------+----------+
=cut

    unless($self->{oneround}) {
	print $so pack("CCCC", 5, 1, 0, 1).inet_aton($self->{destip}).pack("n", $self->{destport});
    }
    $self->{state} = \&es_finalizing;
}

sub es_finalizing($) {
    my $self = shift;
    my $so = $self->{so}; 
    my $buf;

=cut
        +----+-----+-------+------+----------+----------+
        |VER | REP |  RSV  | ATYP | BND.ADDR | BND.PORT |
        +----+-----+-------+------+----------+----------+
        | 1  |  1  | X'00' |  1   | Variable |    2     |
        +----+-----+-------+------+----------+----------+
=cut

    sysread $so, $buf, 10;
    my ($ver, $rep, $rsv, $atyp, $bnd, $bndp) = unpack "CCCCLn", $buf;
    $ver = -1 unless defined $ver;  $rep = -1 unless defined $rep; $atyp = -1 unless defined $atyp;
    unless($ver==5) {
	$self->{errortext} = "socks version $ver after connect? (should be 5)";
	return;
    }
=cut
	     o  X'00' succeeded
             o  X'01' general SOCKS server failure
             o  X'02' connection not allowed by ruleset
             o  X'03' Network unreachable
             o  X'04' Host unreachable
             o  X'05' Connection refused
             o  X'06' TTL expired
             o  X'07' Command not supported
             o  X'08' Address type not supported
=cut

    unless($rep==0) {
	if   ($rep==-1){ $self->{errortext} = "SOCKS5 server suddenly disconnected"; }
	elsif($rep==1){ $self->{errortext} = "general SOCKS5 server failure"; }
	elsif($rep==2){ $self->{errortext} = "SOCKS5 connection not allowed by ruleset"; }
	elsif($rep==3){ $self->{errortext} = "Network unreachable"; }
	elsif($rep==4){ $self->{errortext} = "Host unreachable"; }
	elsif($rep==5){ $self->{errortext} = "Connection refused"; }
	elsif($rep==6){ $self->{errortext} = "TTL expired"; }
	elsif($rep==7){ $self->{errortext} = "SOCKS5 Command not supported"; }
	elsif($rep==8){ $self->{errortext} = "SOCKS5 Address type not supported"; }
	else{ $self->{errortext} = "SOCKS5 reply $rep?"; }
	return;
    }

    $self->{bindip}   = inet_ntoa(pack("L",$bnd));
    $self->{bindport} = $bndp;

    $self->{state} = undef; # finish negotiation
}

sub es_oneround_noauth($);
sub es_oneround_userauth($);

sub establishsocks_init_oneround($) {
    my $self = shift;
    my $so = $self->{so};
    my $user = $self->{souser};
    my $password = $self->{sopassword};

    $self->{oneround}=1;
    unless ($self->{souser}) {
	print $so pack("CCC", 5, 1, 0);
	print $so pack("CCCC", 5, 1, 0, 1).inet_aton($self->{destip}).pack("n", $self->{destport});
        $self->{state} = \&es_noauth;
    } else {
	print $so pack("CCC", 5, 1, 2); 
	print $so pack("CC", 1, length($user)) . $user . pack("C", length($password)) . $password;
	print $so pack("CCCC", 5, 1, 0, 1).inet_aton($self->{destip}).pack("n", $self->{destport});
	$self->{state} = \&es_userauth;
    }
}

