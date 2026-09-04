#!/usr/bin/perl
# Decode captured NIKAI-family TV frames (TCL and friends) into their values.
#
# Usage:
#   curl -s http://<ip>/api/export | perl tools/tcl-decode.pl - tcl
#   perl tools/tcl-decode.pl backup.json [name-substring]
#
# The frame is 24 bits, MSB first, framed as:
#   header mark/space, then 24 * (bit mark, space), then a stop mark.
#
# The trap worth knowing: in NIKAI a ONE is the SHORT space (~1000 us) and a
# ZERO is the LONG one (~2000 us). That is the reverse of NEC and of most
# protocols shaped like this. Decoding with the usual "long space = 1" rule
# gives the exact bitwise complement -- and because these frames carry a
# 12-bit command followed by its own complement, the result still looks
# self-consistent. The complement check below cannot catch it, so the rule is
# simply written down here correctly.
use strict;
use warnings;

my $SHORT_MAX  = 1500;   # midway between the ~1000 one and the ~2000 zero
my $BITS       = 24;
my $FRAME_LEN  = 2 + $BITS * 2 + 1;   # header + bits + stop = 51

# Take the filter off the end, leaving only the file (or "-") for <>.
my $filter = @ARGV > 1 ? lc splice(@ARGV, 1, 1) : '';

my $json = do { local $/; <> };

my @entries = $json =~ /\{"name":(.*?)(?=\{"name":|\]\}\s*$)/gs;

my $shown = 0;
for my $e (@entries) {
  my ($name) = $e =~ /^"([^"]*)"/;
  my ($raw)  = $e =~ /"raw":\[([^\]]*)\]/;
  next unless defined $name && defined $raw;
  next if $filter ne '' && index(lc $name, $filter) < 0;

  my @t = split /\s*,\s*/, $raw;
  if (@t < $FRAME_LEN) {
    printf "%-16s too short for a %d-bit frame (%d timings)\n",
           $name, $BITS, scalar @t;
    $shown++;
    next;
  }

  # A held button repeats the frame; each repeat is one gap plus a frame.
  my @frames;
  for (my $base = 0; $base + $FRAME_LEN <= @t; $base += $FRAME_LEN + 1) {
    my $v = 0;
    for my $i (0 .. $BITS - 1) {
      my $space = $t[$base + 2 + $i * 2 + 1];
      $v = ($v << 1) | ($space < $SHORT_MAX ? 1 : 0);
    }
    push @frames, $v;
  }

  my $v   = $frames[0];
  my $cmd = ($v >> 12) & 0xFFF;
  my $inv = $v & 0xFFF;
  my $ok  = ((~$cmd) & 0xFFF) == $inv ? 'ok' : 'MISMATCH';

  # Every repeat should be identical; a differing one means the capture caught
  # two different presses, and the value cannot be trusted.
  my $stable = 1;
  $stable &&= ($_ == $v) for @frames;

  printf "%-16s 0x%06X   cmd %03X  ~cmd %03X  complement %s  frames %d%s\n",
         $name, $v, $cmd, $inv, $ok, scalar @frames,
         $stable ? '' : '  !! repeats differ';
  $shown++;
}

print "no matching commands\n" unless $shown;
