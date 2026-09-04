#!/usr/bin/perl
# Convert a Flipper-IRDB ".ir" file into codes this firmware can transmit.
#
#   perl tools/flipper-import.pl <file.ir> [--c]
#
# Flipper-IRDB (github.com/Lucaslhm/Flipper-IRDB, CC0-1.0) stores a parsed
# signal as a protocol plus an address and a command, each a little-endian
# byte string. Turning that into something IRremoteESP8266 can send means
# knowing exactly how each protocol packs those fields -- which is the whole
# content of this file.
#
# The rule followed here: where IRremoteESP8266 ships its own encoder, this
# mirrors that encoder rather than the protocol documentation, because the
# encoder is what will actually be transmitting. Where it ships none (RCA),
# the layout was derived from captures and checked against them.
#
# An unrecognised protocol is REFUSED, not guessed. A code emitted from a
# guessed layout looks perfectly fine and does nothing.
use strict;
use warnings;

my ($file, @opts) = @ARGV;
die "usage: flipper-import.pl <file.ir> [--c]\n" unless defined $file;
my $as_c = grep { $_ eq '--c' } @opts;

# --- bit helpers -----------------------------------------------------------

sub rev_bits {
  my ($v, $n) = @_;
  my $r = 0;
  $r = ($r << 1) | (($v >> $_) & 1) for 0 .. $n - 1;
  return $r;
}

sub lsb_bits {
  my ($value, $bits) = @_;
  return join '', map { ($value >> $_) & 1 } 0 .. $bits - 1;
}

# --- per-protocol encoders -------------------------------------------------

# RCA, transmitted as NIKAI.
#
# IRremoteESP8266 has no RCA encoder, but it has NIKAI, and they are the same
# waveform: 4000/4000 header, 500 us mark, spaces of 1000 or 2000. They differ
# only in which space means one --
#
#   RCA    long space (2000) = 1        NIKAI  short space (1000) = 1
#
# -- so an RCA frame is sent by handing NIKAI the bitwise complement. The frame
# is 24 bits: [addr:4][cmd:8][~addr:4][~cmd:8], every field LSB first.
# Verified: reproduces four captures off a real TCL remote exactly.
sub enc_rca {
  my ($addr, $cmd) = @_;
  my $bits = lsb_bits($addr, 4) . lsb_bits($cmd, 8)
           . lsb_bits((~$addr) & 0x0F, 4) . lsb_bits((~$cmd) & 0xFF, 8);
  my $rca = oct('0b' . $bits);
  return ('NIKAI', 24, (~$rca) & 0xFFFFFF);
}

# NEC / Extended NEC -- mirrors IRsend::encodeNEC().
sub enc_nec {
  my ($addr, $cmd) = @_;
  $cmd &= 0xFF;
  $cmd = rev_bits($cmd, 8);
  $cmd = ($cmd << 8) + ($cmd ^ 0xFF);
  if ($addr > 0xFF) {                       # Extended NEC: 16-bit address
    return ('NEC', 32, ((rev_bits($addr, 16) << 16) + $cmd) & 0xFFFFFFFF);
  }
  my $a = rev_bits($addr, 8);
  return ('NEC', 32, (($a << 24) + (($a ^ 0xFF) << 16) + $cmd) & 0xFFFFFFFF);
}

# Samsung32 -- mirrors IRsend::encodeSAMSUNG().
sub enc_samsung {
  my ($addr, $cmd) = @_;
  my $a = rev_bits($addr & 0xFF, 8);
  my $c = rev_bits($cmd & 0xFF, 8);
  return ('SAMSUNG', 32,
          ((($c ^ 0xFF) | ($c << 8) | ($a << 16) | ($a << 24)) & 0xFFFFFFFF));
}

# Sony/SIRC -- mirrors IRsend::encodeSony(). Flipper's address and command map
# straight onto the library's, and the bit count decides the address width.
sub enc_sony {
  my ($addr, $cmd, $bits) = @_;
  my $r;
  if    ($bits == 12) { $r = $addr & 0x1F; }
  elsif ($bits == 15) { $r = $addr & 0xFF; }
  elsif ($bits == 20) { $r = $addr & 0x1F; }   # extended bits unused here
  else { return (); }
  $r = ($r << 7) | ($cmd & 0x7F);
  return ('SONY', $bits, rev_bits($r, $bits));
}

# --- parse -----------------------------------------------------------------

open my $fh, '<', $file or die "cannot open $file: $!\n";
my $text = do { local $/; <$fh> };
close $fh;

my (@rows, %skipped);

while ($text =~ /name:\s*(\S+)[^\n]*\n
                 type:\s*parsed\s*\n
                 protocol:\s*(\S+)\s*\n
                 address:\s*([0-9A-Fa-f]{2})\s+([0-9A-Fa-f]{2})[^\n]*\n
                 command:\s*([0-9A-Fa-f]{2})[^\n]*\n/gx) {
  my ($name, $proto, $a0, $a1, $c0) = ($1, $2, hex($3), hex($4), hex($5));
  my $up = uc $proto;

  # Flipper writes multi-byte fields little-endian, so the second address byte
  # is the high half of an extended address.
  my $addr = $a0 | ($a1 << 8);

  my @out;
  if    ($up eq 'RCA')       { @out = enc_rca($a0, $c0); }
  elsif ($up eq 'NEC')       { @out = enc_nec($a0, $c0); }
  elsif ($up eq 'NECEXT')    { @out = enc_nec($addr > 0xFF ? $addr : $a0, $c0); }
  elsif ($up eq 'SAMSUNG32') { @out = enc_samsung($a0, $c0); }
  elsif ($up eq 'SIRC')      { @out = enc_sony($a0, $c0, 12); }
  elsif ($up eq 'SIRC15')    { @out = enc_sony($a0, $c0, 15); }
  elsif ($up eq 'SIRC20')    { @out = enc_sony($a0, $c0, 20); }
  else                       { $skipped{$up}++; next; }

  unless (@out) { $skipped{$up}++; next; }
  push @rows, [$name, @out];
}

if ($as_c) {
  printf "  /* %-12s */ 0x%08X,   // %s %d bits\n", $_->[0], $_->[3], $_->[1], $_->[2]
    for @rows;
} else {
  printf "%-14s %-8s %2d bits   value=0x%08X\n", @$_[0, 1, 2, 3] for @rows;
}

printf STDERR "\n%d converted", scalar @rows;
printf STDERR ", skipped: %s", join(', ', map { "$_ x$skipped{$_}" } sort keys %skipped)
  if %skipped;
print STDERR "\n";
