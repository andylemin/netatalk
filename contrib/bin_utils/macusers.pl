#!@PERL@

use strict;
use Socket;
use File::Basename;
use vars qw($MAIN_PID $NETATALK_PROCESS $AFPD_PROCESS $PS_STR $MATCH_STR $ASIP_PORT_NO $ASIP_PORT $LSOF);

# Written for linux; may have to be modified for your brand of Unix.
# Support for FreeBSD added by Joe Clarke <jclarke@marcuscom.com>.
# Support Solaris added by Frank Lahm <franklahm@googlemail.com>.
# Support has also been added for 16 character usernames.

if ($ARGV[0] =~ /^(-v|-version|--version)$/) {
    printf("%s \(Netatalk @netatalk_version@\)\n", basename($0));
    exit(1);
} elsif ($ARGV[0] =~ /^(-h|-help|--help)$/) {
    printf("usage: %s \[-v|-version|--version|-h|-help|--help\]\n", basename($0));
    printf("Show users connecting via AFP\n");
    exit(1);
}

$NETATALK_PROCESS = "netatalk";
$AFPD_PROCESS     = "afpd";
if ($^O eq "darwin" || $^O eq "freebsd" || $^O eq "netbsd" || $^O eq "openbsd") {
    $PS_STR    = "-awwxouser,pid,ppid,start,command";
    $MATCH_STR = '(\S+)\s+(\d+)\s+(\d+)\s+([\d\w:]+)';
} elsif ($^O eq "solaris") {
    $PS_STR    = "-eo user,pid,ppid,c,stime,tty,time,comm";
    $MATCH_STR = '\s*(\S+)\s+(\d+)\s+(\d+)\s+\d+\s+([\d\w:]+)';
} else {
    $PS_STR    = "-eo user:32,pid,ppid,c,stime,tty,time,cmd";
    $MATCH_STR = '\s*(\S+)\s+(\d+)\s+(\d+)\s+\d+\s+([\d\w:]+)';
}
$ASIP_PORT    = "afpovertcp";
$ASIP_PORT_NO = 548;

# Change to 0 if you don't have lsof
$LSOF = 1;
my %mac = ();

if ($^O eq "freebsd" || $^O eq "netbsd") {
    open(SOCKSTAT, "sockstat -4 | grep $AFPD_PROCESS | grep -v grep |");

    while (<SOCKSTAT>) {
        next if ($_ !~ /$AFPD_PROCESS/);
        $_ =~ /\S+\s+\S+\s+(\d+)\s+\d+\s+[\w\d]+\s+[\d\.:]+\s+([\d\.]+)/;
        my ($pid, $addr, $host);
        $pid  = $1;
        $addr = $2;
        $host = gethostbyaddr(pack('C4', split(/\./, $addr)), AF_INET);
        ($host) = ($host =~ /(^(\d+\.){3}\d+|[\w\d\-]+)/);
        $mac{$pid} = $host;
    }
    print "PID      UID      Username         Name                 Logintime Mac\n";
    close(SOCKSTAT);
} elsif ($^O eq "solaris") {
    if ($< != 0) {
        print "must be run as root\n";
        exit(1);
    }
    print "PID      UID      Username         Name                 Logintime Mac\n";
} elsif ($LSOF == 1) {
    open(LSOF, "lsof -i :$ASIP_PORT |");

    while (<LSOF>) {
        next if ($_ !~ /$ASIP_PORT/);
        $_ =~ /\w+\s+(\d+).*->([\w\.-]+).*/;
        my ($pid, $host);
        $pid  = $1;
        $host = $2;
        ($host) = ($host =~ /(^(\d+\.){3}\d+|[\w\d\-]+)/);
        $mac{$pid} = $host;
    }
    print "PID      UID      Username         Name                 Logintime Mac\n";
    close(LSOF);
} else {
    print "PID      UID      Username         Name                 Logintime\n";
}

open(PS, "ps $PS_STR |") || die "Unable to open a pipe to ``ps''";

$MAIN_PID = 1;
while (<PS>) {
    next if ($_ !~ /$NETATALK_PROCESS/);
    my ($user, $pid, $ppid, $time, $name, $uid, $t, $ip);
    $_ =~ /$MATCH_STR/;
    $MAIN_PID = $2;
}

close(PS);
open(PS, "ps $PS_STR |") || die "Unable to open a pipe to ``ps''";

while (<PS>) {
    next if ($_ !~ /$AFPD_PROCESS/);
    my ($user, $pid, $ppid, $time, $name, $uid, $t, $ip);
    $_ =~ /$MATCH_STR/;
    $user = $1;
    $pid  = $2;
    $ppid = $3;
    $time = $4;

    if ($ppid != $MAIN_PID) {
        if ($^O eq "solaris") {
            open(PFILES, "pfiles $pid |");
            while (<PFILES>) {
                next if ($_ !~ /port: $ASIP_PORT_NO/);
                while (<PFILES>) {
                    next if ($_ !~ /peername/);
                    if ($_ =~ /AF_INET (.*) port/) {
                        $ip = $1;
                        if ($ip =~ /::ffff:(.*)/) {
                            $ip = $1;
                        }
                    }
                    $mac{$pid} = $ip;
                    last;
                }
                last;
            }
            close(PFILES);
        }

        # Deal with truncated usernames. Caution: this does make the
        # assumption that no username will be all-numeric.
        if ($user =~ /^[0-9]+$/) {
            $uid = $user;
            ($user, $t, $t, $t, $t, $t, $name, $t, $t) = getpwuid($uid);
        } else {
            ($t, $t, $uid, $t, $t, $t, $name, $t, $t) = getpwnam($user);
        }
        ($name) = ($name =~ /(^[^,]+)/);
        printf "%-8d %-8d %-16s %-20s %-9s %s\n", $pid, $uid, $user,
          $name, $time, $mac{$pid};
    }
}

close(PS);
