# $OpenBSD: LaFile.pm,v 1.25 2023/07/06 08:29:26 espie Exp $

# Copyright (c) 2007-2010 Steven Mestdagh <steven@openbsd.org>
# Copyright (c) 2012 Marc Espie <espie@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use v5.36;

package LT::LaFile;
use parent qw(LT::LaLoFile);
use File::Basename;
use LT::Archive;
use LT::Util;
use LT::Trace;

# allows special treatment for some keywords
sub set($self, $k, $v)
{
	$self->SUPER::set($k, $v);
	if ($k eq 'dependency_libs') {
		my @l = split /\s+/, $v;
		$self->{deplib_list} = \@l;
	}
}

sub deplib_list($self)
{
	return $self->{deplib_list}
}

# XXX not sure how much of this cruft we need
sub write($lainfo, $filename, $name)
{
	my $libname = $lainfo->stringize('libname');
	my $sharedlibname = $lainfo->stringize('dlname');
	my $staticlibname = $lainfo->stringize('old_library');
	my $librarynames = $lainfo->stringize('library_names');
	my $deplibs = $lainfo->stringize('dependency_libs');
	my $current = $lainfo->stringize('current');
	my $revision = $lainfo->stringize('revision');
	my $age = $lainfo->stringize('age');
	my $installed = $lainfo->stringize('installed');
	my $shouldnotlink = $lainfo->stringize('shouldnotlink');
	my $libdir = $lainfo->stringize('libdir');

	open(my $la, '>', $filename) or die "Cannot write $filename: $!\n";
	say "creating $filename" if $main::verbose;
	print $la <<EOF
# $name - libtool library file
# Generated by libtool $version
#
# Please DO NOT delete this file!
# It is necessary for linking the library.

# The name that we can dlopen(3).
dlname='$sharedlibname'

# Names of this library.
library_names='$librarynames'

# The name of the static archive.
old_library='$staticlibname'

# Libraries that this one depends upon.
dependency_libs='$deplibs'

# Version information for $libname.
current=$current
age=$age
revision=$revision

# Is this an already installed library?
installed=$installed

# Should we warn about portability when linking against -modules?
shouldnotlink=$shouldnotlink

# Files to dlopen/dlpreopen
dlopen=''
dlpreopen=''

# Directory that this library needs to be installed in:
libdir='$libdir'
EOF
;
}

sub write_shared_libs_log($self, $origv)
{
	if (!defined $ENV{SHARED_LIBS_LOG}) {
	       return;
	}
	my $logfile = $ENV{SHARED_LIBS_LOG};
	my $wantheader = ! -f $logfile;
	open (my $fh, '>>', $logfile) or return;
	my $v = join('.', $self->stringize('current'),
	    $self->stringize('revision'));

	# Remove first leading 'lib', we don't want that in SHARED_LIBS_LOG.
	my $libname = $self->stringize('libname');
	$libname =~ s/^lib//;
	print $fh "# SHARED_LIBS+= <libname>      <obsd version> # <orig version>\n" if $wantheader;
	printf $fh "SHARED_LIBS +=\t%-20s %-8s # %s\n", $libname, $v, $origv;
}

# find .la file associated with a -llib flag
# XXX pick the right one if multiple are found!
sub find($self, $l, $sd)
{
	tsay {"searching .la for $l in $sd"};
	foreach my $la_candidate ("$sd/lib$l.la", "$sd/$l.la") {
		if (-f $la_candidate) {
			tsay {"found $la_candidate"};
			return $la_candidate;
		}
	}
	tsay {".la for $l not found!"};
	return undef;
}

sub install($class, $src, $dstdir, $instprog, $instopts, $strip)
{
	my $srcdir = dirname($src);
	my $srcfile = basename($src);
	my $dstfile = $srcfile;

	my @opts = @$instopts;
	my @stripopts = ('--strip-debug');
	if ($$instprog[-1] =~ m/install([.-](sh|check|wrapper))?$/) {
		push @opts, '-m', '644';
	}

	my $lainfo = $class->parse($src);
	my $sharedlib = $lainfo->{'dlname'};
	my $staticlib = $lainfo->{'old_library'};
	my $laipath = "$srcdir/$ltdir/$srcfile".'i';
	if ($staticlib) {
		# do not strip static libraries, this is done below
		my @realinstopts = @opts;
		@realinstopts = grep { $_ ne '-s' } @realinstopts;
		my $s = "$srcdir/$ltdir/$staticlib";
		my $d = "$dstdir/$staticlib";
		LT::Exec->install(@$instprog, @realinstopts, $s, $d);
		LT::Exec->install('strip', @stripopts, $d) if $strip;
	}
	if ($sharedlib) {
		my $s = "$srcdir/$ltdir/$sharedlib";
		my $d = "$dstdir/$sharedlib";
		LT::Exec->install(@$instprog, @opts, $s, $d);
	}
	if ($laipath) {
		# do not try to strip .la files
		my @realinstopts = @opts;
		@realinstopts = grep { $_ ne '-s' } @realinstopts;
		my $s = $laipath;
		my $d = "$dstdir/$dstfile";
		LT::Exec->install(@$instprog, @realinstopts, $s, $d);
	}
	# for libraries with a -release in their name
	my @libnames = split /\s+/, $lainfo->{library_names};
	foreach my $n (@libnames) {
		next if $n eq $sharedlib;
		unlink("$dstdir/$n");
		symlink($sharedlib, "$dstdir/$n");
	}
}

sub parse($class, $filename)
{
	my $info = $class->SUPER::parse($filename);

	$info->{deplib_list} //= LT::UList->new;
	$info->{libdir} //= '';

	return $info;
}

1;
