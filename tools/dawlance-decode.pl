#!/usr/bin/perl
# Decode captured Dawlance IR frames into their 9 protocol bytes.
#
# Usage:
#   curl -s http://<ip>/api/export | perl tools/dawlance-decode.pl
#   perl tools/dawlance-decode.pl backup.json [name-substring]
#
# The frame is 72 bits, LSB-first within each byte, framed as:
#   header mark/space, then 72 * (bit mark, space), then a stop mark.
# A space longer than the midpoint between the zero and one spaces is a 1.
use strict;
use warnings;

my $ONE_SPACE_MIN = 800;   # midway between the ~390 zero and ~1200 one
my $filter = defined $ARGV[1] ? lc $ARGV[1] : '';

my $json = do { local $/; <> };

# The payload is flat and predictable, so pull the fields out directly rather
# than pulling in a JSON parser that is not installed here.
my @entries = $json =~ /\{"name":(.*?)(?=\{"name":|\]\}\s*$)/gs;

my $shown = 0;
for my $e (@entries) {
  my ($name)  = $e =~ /^"([^"]*)"/;
  my ($group) = $e =~ /"group":"([^"]*)"/;
  my ($acst)  = $e =~ /"acState":(true|false)/;
  my ($raw)   = $e =~ /"raw":\[([^\]]*)\]/;
  next unless defined $name && defined $raw;
  next if $filter ne '' && index(lc $name, $filter) < 0;

  if (defined $acst && $acst eq 'true') {
    printf "%-24s  [generated acState entry, no captured timings]\n", $name;
    $shown++;
    next;
  }

  my @t = split /\s*,\s*/, $raw;
  printf "%-24s  group=%-14s timings=%d\n", $name, ($group // '-'), scalar @t;

  if (@t < 147) {
    print "    !! too short to be a Dawlance frame (want 147)\n\n";
    next;
  }

  # Frames may be concatenated; decode each 147-timing frame we find.
  my $frame = 0;
  for (my $base = 0; $base + 147 <= @t; $base += 147) {
    $frame++;
    my @bytes;
    for my $i (0 .. 8) {
      my $b = 0;
      for my $bit (0 .. 7) {
        my $idx = $base + 2 + (($i * 8 + $bit) * 2) + 1;   # the space
        $b |= (1 << $bit) if $t[$idx] > $ONE_SPACE_MIN;
      }
      push @bytes, $b;
    }

    my $sum = 0;
    $sum += $bytes[$_] for 0 .. 7;
    my $want = ($sum & 0xFF) ^ 0xAA;
    my $ok   = $want == $bytes[8] ? 'ok' : sprintf('BAD (expected %02X)', $want);

    printf "    frame %d: %s   checksum %s\n", $frame,
           join(' ', map { sprintf '%02X', $_ } @bytes), $ok;
    printf "      mode bits %d  power %s  turbo %s  temp %dC  eco %s  light %s\n",
           $bytes[2] & 0x07,
           ($bytes[2] & 0x08) ? 'on' : 'off',
           ($bytes[2] & 0x80) ? 'on' : 'off',
           $bytes[3] + 16,
           ($bytes[5] & 0x01) ? 'on' : 'off',
           ($bytes[5] & 0x80) ? 'on' : 'off';
  }
  print "\n";
  $shown++;
}

print "no matching commands\n" unless $shown;
