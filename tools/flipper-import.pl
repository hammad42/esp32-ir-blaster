#!/usr/bin/perl
# Convert a Flipper-IRDB ".ir" file into codes this firmware can transmit.
#
#   perl tools/flipper-import.pl <file.ir> [--c]
#
# Flipper-IRDB (github.com/Lucaslhm/Flipper-IRDB, CC0-1.0) stores a parsed
# signal as a protocol plus an address and a command. Turning that into
# something IRremoteESP8266 can send means knowing exactly how each protocol
# lays those fields out on the air -- which is the whole content of this file.
#
# RCA -> NIKAI
# ------------
# IRremoteESP8266 has no RCA encoder, but it has NIKAI, and they are the same
# waveform: a 4000/4000 header, a 500 us mark per bit, and a space of 1000 or
# 2000 us. The two differ only in which space means one:
#
#   RCA    long space (2000) = 1
#   NIKAI  short space (1000) = 1
#
# So an RCA frame is sent by handing IRsend the bitwise complement of the RCA
# value and calling it NIKAI. Nothing is lost; it is the same light.
#
# The RCA frame is 24 bits: a 4-bit address, an 8-bit command, then the
# complement of each. Every field goes out LSB first.
#
# Verified against four captures taken off a real TCL remote: netflix, vol_up
# and down reproduce the captured 24-bit values exactly.
use strict;
use warnings;

my ($file, @opts) = @ARGV;
die "usage: flipper-import.pl <file.ir> [--c]\n" unless defined $file;
my $as_c = grep { $_ eq '--c' } @opts;

open my $fh, '<', $file or die "cannot open $file: $!\n";
my $text = do { local $/; <$fh> };
close $fh;

# Emit @p bits of @p value, least significant first.
sub lsb_bits {
  my ($value, $bits) = @_;
  return join '', map { ($value >> $_) & 1 } 0 .. $bits - 1;
}

sub encode_rca {
  my ($addr, $cmd) = @_;
  my $bits = lsb_bits($addr, 4) . lsb_bits($cmd, 8)
           . lsb_bits((~$addr) & 0x0F, 4) . lsb_bits((~$cmd) & 0xFF, 8);
  my $rca = oct('0b' . $bits);
  return ($rca, (~$rca) & 0xFFFFFF);   # (as RCA, as NIKAI)
}

my $skipped = 0;
my @rows;

while ($text =~ /name:\s*(\S+)[^\n]*\n
                 type:\s*parsed\s*\n
                 protocol:\s*(\S+)\s*\n
                 address:\s*([0-9A-Fa-f]{2})[^\n]*\n
                 command:\s*([0-9A-Fa-f]{2})[^\n]*\n/gx) {
  my ($name, $proto, $addr, $cmd) = ($1, $2, hex($3), hex($4));

  if (uc $proto ne 'RCA') {
    # Everything else needs its own field layout worked out and checked
    # against a capture. Refusing is the point: a code emitted from a guessed
    # layout looks fine and does nothing.
    $skipped++;
    next;
  }

  my ($rca, $nikai) = encode_rca($addr, $cmd);
  push @rows, [$name, $addr, $cmd, $rca, $nikai];
}

if ($as_c) {
  printf "  /* %-12s */ 0x%06X,   // RCA addr %02X cmd %02X\n",
         $_->[0], $_->[4], $_->[1], $_->[2] for @rows;
} else {
  printf "%-12s RCA addr=%02X cmd=%02X   air=0x%06X   NIKAI=0x%06X\n",
         @$_[0, 1, 2, 3, 4] for @rows;
}

printf STDERR "\n%d converted, %d skipped (non-RCA)\n", scalar @rows, $skipped;
