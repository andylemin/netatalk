#!@PERL@
#
# AppleSingle/AppleDouble dump
#
# (c) 2009-2012 by HAT <hat@fa2.so-net.ne.jp>
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#

#
# References:
#
# Applesingle and AppleDouble format internals (version 1)
# http://users.phg-online.de/tk/netatalk/doc/Apple/v1/
#
# AppleSingle/AppleDouble Formats for Foreign Files Developer's Note (version2)
# http://users.phg-online.de/tk/netatalk/doc/Apple/v2/AppleSingle_AppleDouble.pdf
#
# Inside Macintosh: Macintosh Toolbox Essentials /
# Chapter 7 - Finder Interface / Finder Interface Reference
# http://developer.apple.com/legacy/mac/library/documentation/mac/toolbox/Toolbox-463.html
#
# Finder Interface Reference
# http://developer.apple.com/legacy/mac/library/documentation/Carbon/Reference/Finder_Interface/Reference/reference.html
#
# Technical Note TN1150  HFS Plus Volume Format
# http://developer.apple.com/mac/library/technotes/tn/tn1150.html#FinderInfo
#
# CarbonHeaders source
# http://www.opensource.apple.com/source/CarbonHeaders/CarbonHeaders-8A428/Finder.h
# http://www.opensource.apple.com/source/CarbonHeaders/CarbonHeaders-9A581/Finder.h
#
# Xcode 3.2.1
# /usr/bin/SetFile
# /usr/bin/GetFileInfo
#
# Mac OS X 10.6.2 kernel source
# http://www.opensource.apple.com/source/xnu/xnu-1486.2.11/bsd/vfs/vfs_xattr.c
#

use strict;
use File::Basename;
use File::Spec;
use File::Temp qw /tempfile/;
# require perl >= 5.8
use bigint;
use IPC::Open2 qw /open2/;

# check command for extended attributes -----------------------------------
my $eacommand;
if (0 == system("which getfattr > /dev/null 2>&1")) {
    $eacommand = 1;
} elsif (0 == system("which attr > /dev/null 2>&1")) {
    $eacommand = 2;
} elsif (0 == system("which runat > /dev/null 2>&1")) {
    $eacommand = 3;
} elsif (0 == system("which getextattr > /dev/null 2>&1")) {
    $eacommand = 4;
} else {
    $eacommand = 0;
}

#printf ( "eacommand = %d\n", $eacommand );   # debug

# parse command line -----------------------------------------------

my $stdinputmode = 0;
my $eaoption     = 0;
#  0: unknown   1: file   2: directory
my $finderinfo = 0;
my $afile;
while (my $arg = shift @ARGV) {
    if ($arg =~ /^(-h|-help|--help)$/) {
        printf("usage: %s [-a] [FILE|DIR]\n",       basename($0));
        printf(" or:   %s -e FILE|DIR\n",           basename($0));
        printf(" or:   %s -f [FILE]\n",             basename($0));
        printf(" or:   %s -d [FILE]\n",             basename($0));
        printf(" or:   %s -h|-help|--help\n",       basename($0));
        printf(" or:   %s -v|-version|--version\n", basename($0));
        printf("Dump AppleSingle/AppleDouble format data.\n");
        printf("With no FILE|DIR, or when FILE|DIR is -, read standard input.\n");
        printf("\n");
        printf("  -a (default)     Dump a AppleSingle/AppleDouble data for FILE or DIR\n");
        printf("                   automatically.\n");
        printf("                   If FILE is not AppleSingle/AppleDouble format,\n");
        printf("                   look for extended attribute, .AppleDouble/FILE and ._FILE.\n");
        printf("                   If DIR, look for extended attribute,\n");
        printf("                   DIR/.AppleDouble/.Parent and ._DIR.\n");
        printf("  -e               Dump extended attribute of FILE or DIR\n");
        printf("  -f               Dump FILE. Assume FinderInfo to be FileInfo.\n");
        printf("  -d               Dump FILE. Assume FinderInfo to be DirInfo.\n");
        printf("  -h,-help,--help  Display this help and exit\n");
        printf("  -v,-version,--version  Show version and exit\n");
        printf("\n");
        printf("There is no way to detect whether FinderInfo is FileInfo or DirInfo.\n");
        printf("By default, %s examins whether file or directory,\n", basename($0));
        printf("a parent directory is .AppleDouble, filename is ._*, filename is .Parent,\n");
        printf("and so on.\n");
        printf("If setting option -e, -f or -d, %s assume FinderInfo and doesn't look for\n");
        printf("another file.\n");
        exit 1;
    } elsif ($arg =~ /^(-v|-version|--version)$/) {
        printf("%s \(Netatalk @netatalk_version@\)\n", basename($0));
        exit 1;
    } elsif ($arg eq "-a") {
        $finderinfo = 0;
    } elsif ($arg eq "-e") {
        if ($eacommand == 0) {
            printf(STDERR "%s: unsupported option -e\n", basename($0));
            printf(STDERR "because neither getfattr, attr, runat nor getextattr is found.\n");
            exit 1;
        }
        $eaoption = 1;
    } elsif ($arg eq "-f") {
        $finderinfo = 1;
    } elsif ($arg eq "-d") {
        $finderinfo = 2;
    } elsif ($arg eq "-") {
        $stdinputmode = 1;
    } elsif ($arg =~ /^-/) {
        printf(STDERR "%s: invalid option %s\n", basename($0), $arg);
        printf(STDERR "Try \`%s\ -h' for more information.\n", basename($0));
        exit 1;
    } else {
        $afile = $arg;
    }
}

if (!($afile)) {
    $stdinputmode = 1;
} elsif (!(-e $afile)) {
    printf(STDERR "\"%s\" is not found.\n", $afile);
    exit 1;
}

# detect FinderInfo, and search AppleSingle/AppleDouble file --------------

my $abspath = File::Spec->rel2abs($afile);
my ($basename, $path, $ext) = fileparse($abspath);

my $openfile;
my $openmessage;
my $buf;
if ($stdinputmode == 1) {
    my $eatempfh;
    ($eatempfh, $openfile) = tempfile(UNLINK => 1);
    system("cat - > $openfile");
    close($eatempfh);
    $openmessage = "Dumping Standard Input...\n";
} elsif ($eaoption == 1) {
    if (-f $afile) {
        $finderinfo = 1;
    } elsif (-d $afile) {
        $finderinfo = 2;
    } else {
        printf(STDERR "unknown error: %s\n", $afile);
        exit 1;
    }
    if (0 == checkea($afile)) {
        printf(STDERR "\"%s\"'s extended attribute is not found\n", $afile);
        exit 1;
    }
    $openfile    = eaopenfile($afile);
    $openmessage = "Dumping \"$afile\"'s extended attribute...\n";
} elsif ($finderinfo != 0) {
    $openfile    = $afile;
    $openmessage = "Dumping \"$openfile\"...\n";
} elsif (-f $afile) {
    if ($basename eq ".Parent") {
        $finderinfo = 2;
    } elsif ($path =~ /\/.AppleDouble\/$/) {
        $finderinfo = 1;
    } elsif ($basename =~ /^._/) {
        if (-f $path . substr($basename, 2)) {
            $finderinfo = 1;
        } elsif (-d $path . substr($basename, 2)) {
            $finderinfo = 2;
        }
    }
    if (!open(INFILE, "<$afile")) {
        printf(STDERR "cannot open %s\n", $afile);
        exit 1;
    }
    read(INFILE, $buf, 4);
    my $val = unpack("N", $buf);
    close(INFILE);
    if ($val == 0x00051600 || $val == 0x00051607) {
        $openfile    = $afile;
        $openmessage = "Dumping \"$openfile\"...\n";
    } else {
        printf("\"%s\" is not AppleSingle/AppleDouble format.\n", $afile);
        $finderinfo = 1;
        my $adcount      = 0;
        my $netatalkfile = $path . ".AppleDouble/" . $basename;
        my $osxfile      = $path . "._" . $basename;

        if (1 == checkea($afile)) {
            printf("\"%s\"\'s extended attribute is found.\n", $afile);
            $adcount++;
            $openfile    = eaopenfile($afile);
            $openmessage = "Dumping \"$afile\"'s extended attribute...\n";
        }
        if (-e $netatalkfile) {
            printf("\"%s\" is found.\n", $netatalkfile);
            $adcount++;
            $openfile    = $netatalkfile;
            $openmessage = "Dumping \"$openfile\"...\n";
        }
        if (-e $osxfile) {
            printf("\"%s\" is found.\n", $osxfile);
            $adcount++;
            $openfile    = $osxfile;
            $openmessage = "Dumping \"$openfile\"...\n";
        }
        if ($adcount == 0) {
            printf("AppleSingle/AppleDouble data is not found.\n");
            exit 1;
        }
        if ($adcount != 1) {
            printf("Specify any one.\n");
            exit 1;
        }
    }
} elsif (-d $afile) {
    printf("\"%s\" is a directory.\n", $afile);
    $finderinfo = 2;
    my $adcount      = 0;
    my $netatalkfile = $path . $basename . "/.AppleDouble/.Parent";
    my $osxfile      = $path . "._" . $basename;

    if (1 == checkea($afile)) {
        printf("\"%s\"\'s extended attribute is found.\n", $afile);
        $adcount++;
        $openfile    = eaopenfile($afile);
        $openmessage = "Dumping \"$afile\"'s extended attribute...\n";
    }
    if (-e $netatalkfile) {
        printf("\"%s\" is found.\n", $netatalkfile);
        $adcount++;
        $openfile    = $netatalkfile;
        $openmessage = "Dumping \"$openfile\"...\n";
    }
    if (-e $osxfile) {
        printf("\"%s\" is found.\n", $osxfile);
        $adcount++;
        $openfile    = $osxfile;
        $openmessage = "Dumping \"$openfile\"...\n";
    }
    if ($adcount == 0) {
        printf("AppleSingle/AppleDouble data is not found.\n");
        exit 1;
    }
    if ($adcount != 1) {
        printf("Specify any one.\n");
        exit 1;
    }
} else {
    printf(STDERR "unknown error: %s\n", $afile);
    exit 1;
}

if (!open(INFILE, "<$openfile")) {
    printf(STDERR "cannot open %s\n", $openfile);
    exit 1;
}

printf($openmessage);

#Dump --------------------------------------------------------

# Magic Number -----------------------------------------------

print "-------------------------------------------------------------------------------\n";

read(INFILE, $buf, 4);
my $val = unpack("N", $buf);
printf("MagicNumber: %08X", $val);
if ($val == 0x00051600) {
    printf("                                        : AppleSingle");
} elsif ($val == 0x00051607) {
    printf("                                        : AppleDouble");
} else {
    printf("                                        : Unknown");
}
print "\n";

# Version Number ---------------------------------------------

read(INFILE, $buf, 4);
my $version = unpack("N", $buf);
printf("Version    : %08X", $version);
if ($version == 0x00010000) {
    printf("                                        : Version 1");
} elsif ($version == 0x00020000) {
    printf("                                        : Version 2");
} else {
    printf("                                        : Unknown");
}
print "\n";

# v1:Home file system / v2:Filler ----------------------------

read(INFILE, $buf, 16);
if ($version == 0x00010000) {
    print "HomeFileSys:";
} else {
    print "Filler     :";
}
hexdump($buf, 16, 16, " ");

# Number of entities -----------------------------------------

read(INFILE, $buf, 2);
my $entnum = unpack("n", $buf);
printf("Num. of ent: %04X    ",                        $entnum);
printf("                                        : %d", $entnum);
print "\n";

# data -------------------------------------------------------

for (my $num = 0 ; $num < $entnum ; $num++) {

    seek(INFILE, ($num * 12 + 26), 0);

    #    Entry ---------------------------------------------------

    read(INFILE, $buf, 4);
    my $entid = unpack("N", $buf);
    print "\n-------------------------------------------------------------------------------\n";
    printf("Entry ID   : %08X", $entid);
    if    ($entid == 1)          { printf(" : Data Fork"); }
    elsif ($entid == 2)          { printf(" : Resource Fork"); }
    elsif ($entid == 3)          { printf(" : Real Name"); }
    elsif ($entid == 4)          { printf(" : Comment"); }
    elsif ($entid == 5)          { printf(" : Icon, B&W"); }
    elsif ($entid == 6)          { printf(" : Icon Color"); }
    elsif ($entid == 7)          { printf(" : File Info"); }
    elsif ($entid == 8)          { printf(" : File Dates Info"); }
    elsif ($entid == 9)          { printf(" : Finder Info"); }
    elsif ($entid == 10)         { printf(" : Macintosh File Info"); }
    elsif ($entid == 11)         { printf(" : ProDOS File Info"); }
    elsif ($entid == 12)         { printf(" : MS-DOS File Info"); }
    elsif ($entid == 13)         { printf(" : Short Name"); }
    elsif ($entid == 14)         { printf(" : AFP File Info"); }
    elsif ($entid == 15)         { printf(" : Directory ID"); }
    elsif ($entid == 0x8053567E) { printf(" : CNID (Netatalk Extended)"); }
    elsif ($entid == 0x8053594E) { printf(" : DB stamp (Netatalk Extended)"); }
    elsif ($entid == 0x80444556) { printf(" : dev (Netatalk Extended)"); }
    elsif ($entid == 0x80494E4F) { printf(" : inode (Netatalk Extended)"); }
    else                         { printf(" : Unknown"); }
    print "\n";

    #    Offset -------------------------------------------------

    read(INFILE, $buf, 4);
    my $ofst = unpack("N", $buf);
    printf("Offset     : %08X", $ofst);
    printf(" : %d ",            $ofst);

    #    Length -------------------------------------------------

    read(INFILE, $buf, 4);
    my $len = unpack("N", $buf);
    printf("\nLength     : %08X", $len);
    printf(" : %d",               $len);
    my $quo = $len >> 4;
    my $rem = $len & 0xF;
    print "\n";

    #     Dump for each Entry ID --------------------------------

    #    if ( $entid ==  1 ) { ; } # Data Fork
    #    if ( $entid ==  2 ) { ; } # Resource Fork
    #    if ( $entid ==  3 ) { ; } # Real Name
    #    if ( $entid ==  4 ) { ; } # Comment
    #    if ( $entid ==  5 ) { ; } # Icon, B&W
    #    if ( $entid ==  6 ) { ; } # Icon Color
    #    if ( $entid ==  7 ) { ; } # File Info
    if    ($entid == 8) { filedatesdump($ofst, $len); }
    elsif ($entid == 9) { finderinfodump($ofst, $len); }
    #    if ( $entid == 10 ) { ; } # Macintosh File Info
    #    if ( $entid == 11 ) { ; } # ProDOS File Info
    #    if ( $entid == 12 ) { ; } # MS-DOS File Info
    #    if ( $entid == 13 ) { ; } # Short Name
    #    if ( $entid == 14 ) { ; } # AFP File Info
    elsif ($entid == 15) {
        # Directory ID
        print "\n";
        bedump($ofst, $len);
    } elsif ($entid == 0x8053567E) {
        # CNID (Netatalk Extended)
        print "\n";
        bedump($ofst, $len);
        ledump($ofst, $len);
    } elsif ($entid == 0x8053594E) {
        # DB stamp (Netatalk Extended)
        print "\n";
        bedump($ofst, $len);
        ledump($ofst, $len);
    } elsif ($entid == 0x80444556) {
        # dev (Netatalk Extended)
        print "\n";
        bedump($ofst, $len);
        ledump($ofst, $len);
    } elsif ($entid == 0x80494E4F) {
        # inode (Netatalk Extended)
        print "\n";
        bedump($ofst, $len);
        ledump($ofst, $len);
    }

    #    RAW Dump ---------------------------------------------------

    if (($quo > 0) || ($rem > 0)) {
        print "\n";
        print "-RAW DUMP--:  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F : (ASCII)\n";
    }

    seek(INFILE, $ofst, 0);
    rawdump($quo, $rem);

}

close(INFILE);
exit 0;

#sub -----------------------------------------------------------

sub filedatesdump {
    my ($ofst, $len) = @_;
    my ($datedata, $datestr, $buf);

    my @datetype = ('create    ', 'modify    ', 'backup    ', 'access    ');

    seek(INFILE, $ofst, 0);

    print "\n";
    printf("-DATE------:          : (GMT)                    : (Local)\n");

    for (my $i = 0 ; $i < 4 ; $i++) {
        read(INFILE, $buf, 4);
        $datedata = unpack("N", $buf);
        if ($datedata < 0x80000000) {
            $datestr = gmtime($datedata + 946684800) . " : " . localtime($datedata + 946684800);
        } elsif ($datedata == 0x80000000) {
            $datestr = "Unknown or Initial";
        } else {
            $datestr = gmtime($datedata - 3348282496) . " : " . localtime($datedata - 3348282496);
        }
        printf("%s : %08X : %s\n", $datetype[$i], $datedata, $datestr);
    }
}

sub finderinfodump {
    my ($ofst, $len) = @_;
    my $buf;

    seek(INFILE, $ofst, 0);

    if ($finderinfo == 0) {
        print "\n";
        print "-NOTE------: cannot detect whether FInfo or DInfo. assume FInfo.\n";
    }

    if ($finderinfo == 0 || $finderinfo == 1) {
        filefinderinfodump();
    } elsif ($finderinfo == 2) {
        dirfinderinfodump();
    } else {
        print STDERR "unknown FinderInfo type\n";
    }

    if ($len > 32) { eadump(); }
}

sub filefinderinfodump {
    my $buf;

    print "\n";
    print "-FInfo-----:\n";

    read(INFILE, $buf, 4);
    print "Type       : ";
    hexdump($buf, 4, 4, "");

    read(INFILE, $buf, 4);
    print "Creator    : ";
    hexdump($buf, 4, 4, "");

    flagsdump();

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Location v : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Location h : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    print "Fldr       : ";
    hexdump($buf, 2, 4, "");

    print "\n";
    print "-FXInfo----:\n";

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rsvd|IconID: %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    print "Rsvd       : ";
    hexdump($buf, 2, 4, "");
    read(INFILE, $buf, 2);
    print "Rsvd       : ";
    hexdump($buf, 2, 4, "");
    read(INFILE, $buf, 2);
    print "Rsvd       : ";
    hexdump($buf, 2, 4, "");

    xflagsdump();

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rsvd|commnt: %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 4);
    $val = unpack("N", $buf);
    printf("PutAway    : %08X", $val);
    # Why SInt32?
    printf(" : %d\n", $val > 0x7FFFFFFF ? $val - 0x100000000 : $val);

}

sub dirfinderinfodump {
    my $buf;

    print "\n";
    print "-DInfo-----:\n";

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rect top   : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rect left  : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rect bottom: %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Rect right : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    flagsdump();

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Location v : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Location h : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    print "View       : ";
    hexdump($buf, 2, 4, "");

    print "\n";
    print "-DXInfo----:\n";

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Scroll v   : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 2);
    $val = unpack("n", $buf);
    printf("Scroll h   : %04X", $val);
    printf("     : %d\n",       $val > 0x7FFF ? $val - 0x10000 : $val);

    read(INFILE, $buf, 4);
    $val = unpack("N", $buf);
    printf("Rsvd|OpnChn: %08X", $val);
    # Why SInt32?
    printf(" : %d\n", $val > 0x7FFFFFFF ? $val - 0x100000000 : $val);

    xflagsdump();

    read(INFILE, $buf, 2);
    print "Comment    : ";
    hexdump($buf, 2, 4, "");

    read(INFILE, $buf, 4);
    $val = unpack("N", $buf);
    printf("PutAway    : %08X", $val);
    # Why SInt32?
    printf(" : %d\n", $val > 0x7FFFFFFF ? $val - 0x100000000 : $val);

}

sub flagsdump {
    my $buf;

    my @colortype = ('none', 'gray', 'green', 'purple', 'blue', 'yellow', 'red', 'orange');

    read(INFILE, $buf, 2);
    my $flags = unpack("n", $buf);
    printf("isAlias    : %d\n", ($flags >> 15) & 1);
    printf("Invisible  : %d\n", ($flags >> 14) & 1);
    printf("hasBundle  : %d\n", ($flags >> 13) & 1);
    printf("nameLocked : %d\n", ($flags >> 12) & 1);
    printf("Stationery : %d\n", ($flags >> 11) & 1);
    printf("CustomIcon : %d\n", ($flags >> 10) & 1);
    printf("Reserved   : %d\n", ($flags >> 9) & 1);
    printf("Inited     : %d\n", ($flags >> 8) & 1);
    printf("NoINITS    : %d\n", ($flags >> 7) & 1);
    printf("Shared     : %d\n", ($flags >> 6) & 1);
    printf("SwitchLaunc: %d\n", ($flags >> 5) & 1);
    printf("Hidden Ext : %d\n", ($flags >> 4) & 1);
    printf(
           "color      : %d%d%d      : %s\n", ($flags >> 3) & 1,
           ($flags >> 2) & 1,
           ($flags >> 1) & 1,
           @colortype[($flags & 0xE) >> 1]
    );
    printf("isOnDesk   : %d\n", ($flags >> 0) & 1);

}

sub xflagsdump {
    my $buf;

    read(INFILE, $buf, 2);
    my $flags = unpack("n", $buf);

    if (($flags >> 15) == 1) {
        print "Script     : ";
        hexdump($buf, 1, 4, "");
    } else {
        printf("AreInvalid : %d\n", ($flags >> 15) & 1);
        printf("unknown bit: %d\n", ($flags >> 14) & 1);
        printf("unknown bit: %d\n", ($flags >> 13) & 1);
        printf("unknown bit: %d\n", ($flags >> 12) & 1);
        printf("unknown bit: %d\n", ($flags >> 11) & 1);
        printf("unknown bit: %d\n", ($flags >> 10) & 1);
        printf("unknown bit: %d\n", ($flags >> 9) & 1);
    }

    printf("CustomBadge: %d\n", ($flags >> 8) & 1);
    printf("ObjctIsBusy: %d\n", ($flags >> 7) & 1);
    printf("unknown bit: %d\n", ($flags >> 6) & 1);
    printf("unknown bit: %d\n", ($flags >> 5) & 1);
    printf("unknown bit: %d\n", ($flags >> 4) & 1);
    printf("unknown bit: %d\n", ($flags >> 3) & 1);
    printf("RoutingInfo: %d\n", ($flags >> 2) & 1);
    printf("unknown bit: %d\n", ($flags >> 1) & 1);
    printf("unknown bit: %d\n", ($flags >> 0) & 1);

}

sub eadump {
    my $buf;

    print "\n";
    print "-EA--------:\n";

    read(INFILE, $buf, 2);
    print "pad        : ";
    hexdump($buf, 2, 4, "");

    read(INFILE, $buf, 4);
    print "magic      : ";
    hexdump($buf, 4, 4, "");

    read(INFILE, $buf, 4);
    my $ea_debug_tag = unpack("N", $buf);
    printf("debug_tag  : %08X", $ea_debug_tag);
    printf(" : %d\n",           $ea_debug_tag);

    read(INFILE, $buf, 4);
    my $ea_total_size = unpack("N", $buf);
    printf("total_size : %08X", $ea_total_size);
    printf(" : %d\n",           $ea_total_size);

    read(INFILE, $buf, 4);
    my $ea_data_start = unpack("N", $buf);
    printf("data_start : %08X", $ea_data_start);
    printf(" : %d\n",           $ea_data_start);

    read(INFILE, $buf, 4);
    my $ea_data_length = unpack("N", $buf);
    printf("data_length: %08X", $ea_data_length);
    printf(" : %d\n",           $ea_data_length);

    read(INFILE, $buf, 4);
    print "reserved[0]: ";
    hexdump($buf, 4, 4, "");

    read(INFILE, $buf, 4);
    print "reserved[1]: ";
    hexdump($buf, 4, 4, "");

    read(INFILE, $buf, 4);
    print "reserved[2]: ";
    hexdump($buf, 4, 4, "");

    read(INFILE, $buf, 2);
    print "flags      : ";
    hexdump($buf, 2, 4, "");

    read(INFILE, $buf, 2);
    my $ea_num_attrs = unpack("n", $buf);
    printf("num_attrs  : %04X", $ea_num_attrs);
    printf("     : %d\n",       $ea_num_attrs);

    my $pos = tell(INFILE);

    for (my $i = 0 ; $i < $ea_num_attrs ; $i++) {

        $pos = (($pos & 0x3) == 0) ? ($pos) : ((($pos >> 2) + 1) << 2);
        seek(INFILE, $pos, 0);

        print "-EA ENTRY--:\n";

        read(INFILE, $buf, 4);
        my $ea_offset = unpack("N", $buf);
        printf("offset     : %08X", $ea_offset);
        printf(" : %d\n",           $ea_offset);

        read(INFILE, $buf, 4);
        my $ea_length = unpack("N", $buf);
        printf("length     : %08X", $ea_length);
        printf(" : %d\n",           $ea_length);

        read(INFILE, $buf, 2);
        print "flags      : ";
        hexdump($buf, 2, 4, "");

        read(INFILE, $buf, 1);
        my $ea_namelen = unpack("C", $buf);
        printf("namelen    : %02X", $ea_namelen);
        printf("       : %d\n",     $ea_namelen);

        my $ea_namequo = $ea_namelen >> 4;
        my $ea_namerem = $ea_namelen & 0xF;
        print "-EA NAME---:  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F : (ASCII)\n";
        rawdump($ea_namequo, $ea_namerem);

        $pos = tell(INFILE);

        seek(INFILE, $ea_offset, 0);
        my $ea_quo = $ea_length >> 4;
        my $ea_rem = $ea_length & 0xF;
        print "-EA VALUE--:  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F : (ASCII)\n";
        rawdump($ea_quo, $ea_rem);
    }
}

sub bedump {
    my ($ofst, $len) = @_;
    my ($i, $value, $buf, @bytedata);

    seek(INFILE, $ofst, 0);

    printf("%2dbit-BE   : ", $len * 8);

    $value = 0;
    for ($i = 0 ; $i < $len ; $i++) {
        read(INFILE, $buf, 1);
        $bytedata[$i] = unpack("C", $buf);
        $value += $bytedata[$i] << (($len - $i - 1) * 8);
    }

    for ($i = 0 ; $i < $len ; $i++) {
        printf("%02X", $bytedata[$i]);
    }

    printf(" : %s", $value);
    print "\n";
}

sub ledump {
    my ($ofst, $len) = @_;
    my ($i, $value, $buf, @bytedata);

    seek(INFILE, $ofst, 0);

    printf("%2dbit-LE   : ", $len * 8);

    $value = 0;
    for ($i = 0 ; $i < $len ; $i++) {
        read(INFILE, $buf, 1);
        $bytedata[$len - $i - 1] = unpack("C", $buf);
        $value += $bytedata[$len - $i - 1] << ($i * 8);
    }

    for ($i = 0 ; $i < $len ; $i++) {
        printf("%02X", $bytedata[$i]);
    }

    printf(" : %s", $value);
    print "\n";
}

sub rawdump {
    my ($quo, $rem) = @_;
    my ($addrs, $line, $buf);

    $addrs = 0;
    for ($line = 0 ; $line < $quo ; $line++) {
        read(INFILE, $buf, 16);
        printf("%08X   :", $addrs);
        hexdump($buf, 16, 16, " ");
        $addrs = $addrs + 0x10;
    }
    if ($rem != 0) {
        read(INFILE, $buf, $rem);
        printf("%08X   :", $addrs);
        hexdump($buf, $rem, 16, " ");
    }
}

sub hexdump {
    my ($buf, $len, $col, $delimit) = @_;
    my $i;

    my $hexstr = "";
    my $ascstr = "";

    for ($i = 0 ; $i < $len ; $i++) {
        $val = substr($buf, $i, 1);
        my $ascval = ord($val);
        $hexstr .= sprintf("%s%02X", $delimit, $ascval);

        if (($ascval < 32) || ($ascval > 126)) {
            $val = ".";
        }
        $ascstr .= $val;
    }
    for (; $i < $col ; $i++) {
        $hexstr .= "  " . $delimit;
        $ascstr .= " ";
    }

    printf("%s : %s", $hexstr, $ascstr);

    print "\n";
}

sub checkea {
    my ($file) = @_;

    if ($eacommand == 1) {
        open2(\*EALIST, \*EAIN, 'getfattr', $file) or die $@;
        while (<EALIST>) {
            if ($_ eq "user.org.netatalk.Metadata\n") {
                close(EALIST);
                close(EAIN);
                return 1;
            }
        }
        close(EALIST);
        close(EAIN);
        return 0;
    } elsif ($eacommand == 2) {
        open2(\*EALIST, \*EAIN, 'attr', '-q', '-l', $file) or die $@;
        while (<EALIST>) {
            if ($_ eq "org.netatalk.Metadata\n") {
                close(EALIST);
                close(EAIN);
                return 1;
            }
        }
        close(EALIST);
        close(EAIN);
        return 0;
    } elsif ($eacommand == 3) {
        open2(\*EALIST, \*EAIN, 'runat', $file, 'ls', '-1') or die $@;
        while (<EALIST>) {
            if ($_ eq "org.netatalk.Metadata\n") {
                close(EALIST);
                close(EAIN);
                return 1;
            }
        }
        close(EALIST);
        close(EAIN);
        return 0;
    } elsif ($eacommand == 4) {
        open2(\*EALIST, \*EAIN, 'lsextattr', '-q', 'user', $file) or die $@;
        while (<EALIST>) {
            $_ = "\t" . $_;
            if ($_ =~ /\torg\.netatalk\.Metadata[\n\t]/) {
                close(EALIST);
                close(EAIN);
                return 1;
            }
        }
        close(EALIST);
        close(EAIN);
        return 0;
    } else {
        return 0;
    }
}

sub eaopenfile {
    my ($file) = @_;
    my @eacommands = ();

    if ($eacommand == 1) {
        @eacommands = ('getfattr', '--only-values', '-n', 'user.org.netatalk.Metadata', $file);
    } elsif ($eacommand == 2) {
        @eacommands = ('attr', '-q', '-g', 'org.netatalk.Metadata', $file);
    } elsif ($eacommand == 3) {
        @eacommands = ('runat', $file, 'cat', 'org.netatalk.Metadata',);
    } elsif ($eacommand == 4) {
        @eacommands = ('getextattr', '-q', 'user', 'org.netatalk.Metadata', $file);
    } else {
        return "";
    }

    my ($eatempfh, $eatempfile) = tempfile(UNLINK => 1);
    open2(my $ealist, my $eain, @eacommands) or die $@;
    print $eatempfh $_ while (<$ealist>);
    close($ealist, $eain);
    close($eatempfh);
    return $eatempfile;
}

#EOF
