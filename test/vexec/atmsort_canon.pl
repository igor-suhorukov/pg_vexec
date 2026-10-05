#!/usr/bin/perl
# SPDX-License-Identifier: Apache-2.0
#
# atmsort_canon.pl <gpdiff dir> [--gpd_init <file>]... < results > canonical
#
# A results file of Cloudberry's tests as Cloudberry's gpdiff.pl reads it
# before it compares (pg_vector_executor.md §6.1): through atmsort.pm, with
# the init files' matchsubs and matchignores, the test's own, its
# start_ignore blocks marked GP_IGNORE:, its "-- order" directives, unordered
# results sorted, and plans left out (gpdiff's --gpd_ignore_plans).  The
# port's greenplum suite keeps atmsort.pm, explain.pm and GPTest.pm in its
# work directory's gpdiff/; portsuites.sh then compares the sessions'
# canonical files, as gpdiff compares a test with its expected output.
use strict;
use warnings;

my $dir = shift @ARGV or die "usage: atmsort_canon.pl <gpdiff dir> [--gpd_init <file>]...\n";
my @inits;
while (@ARGV)
{
	my $o = shift @ARGV;
	die "atmsort_canon.pl: unknown argument $o\n" unless $o eq '--gpd_init' && @ARGV;
	push @inits, shift @ARGV;
}
unshift @INC, $dir;
require atmsort;

atmsort::atmsort_init(IGNORE_PLANS => 1, INIT_FILES => \@inits);
atmsort::run_fhs(\*STDIN, \*STDOUT);
