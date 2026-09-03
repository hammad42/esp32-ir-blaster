#!/usr/bin/env bash
# Back up and restore learned IR commands over the REST API.
#
# A filesystem OTA erases every stored command -- the docs say to back up first,
# but until now the only way to do it was clicking through the browser. This is
# the same thing from a terminal, so it can go in a script next to the upload.
#
#   tools/ir-backup.sh save    <host> [file]   # default: ir-backup.json
#   tools/ir-backup.sh restore <host> [file]
#   tools/ir-backup.sh list    <file>
#
#   host may be an IP or a name: 192.168.1.42, ir-blaster.local
#   set IR_AUTH=user:pass if the device has authentication enabled
#
# Restore posts one command per request, because that is what the device
# accepts: a whole archive will not fit in its RAM. Existing names are
# rejected by the firmware and reported here rather than silently skipped.
set -uo pipefail

usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[ $# -ge 2 ] || usage

cmd=$1
auth=()
[ -n "${IR_AUTH:-}" ] && auth=(-u "$IR_AUTH")

# Splits the archive into one file per command. Each object runs from its
# "name" key to the close of its raw array, which is the only nesting inside.
split_commands() {
  perl -0ne '
    my $dir = $ARGV[0] // ".";
    my $i = 0;
    while (/(\{"name":.*?\]\})/gs) {
      $i++;
      open(my $o, ">", "$ENV{OUTDIR}/cmd_$i.json") or die $!;
      print $o $1; close $o;
    }
    print "$i\n";
  ' "$1"
}

case "$cmd" in
  save)
    host=$2; file=${3:-ir-backup.json}
    echo "backing up from $host ..."
    if ! curl -fsS -m 60 "${auth[@]}" "http://$host/api/export" -o "$file"; then
      echo "export failed -- is $host reachable?" >&2; exit 1
    fi
    n=$(grep -o '"name":"' "$file" | wc -l | tr -d ' ')
    bytes=$(wc -c < "$file" | tr -d ' ')
    echo "saved $n command(s), $bytes bytes -> $file"
    ;;

  restore)
    host=$2; file=${3:-ir-backup.json}
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
    OUTDIR=$(mktemp -d); export OUTDIR
    trap 'rm -rf "$OUTDIR"' EXIT
    total=$(split_commands "$file")
    [ "$total" -gt 0 ] || { echo "no commands found in $file" >&2; exit 1; }

    echo "restoring $total command(s) to $host ..."
    ok=0; failed=0
    for i in $(seq 1 "$total"); do
      name=$(perl -0ne 'print $1 if /"name":"(.*?)"/' "$OUTDIR/cmd_$i.json")
      printf '  %2d/%s  %-24s ' "$i" "$total" "$name"
      reply=$(curl -fsS -m 30 "${auth[@]}" -X POST \
                -H 'Content-Type: application/json' \
                --data-binary @"$OUTDIR/cmd_$i.json" \
                "http://$host/api/import" 2>&1)
      case "$reply" in
        *'"ok":true'*) echo "ok";      ok=$((ok+1)) ;;
        *)             echo "FAILED: $reply"; failed=$((failed+1)) ;;
      esac
    done
    echo "restored $ok, failed $failed"
    [ "$failed" -eq 0 ] || exit 1
    ;;

  list)
    file=$2
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
    perl -0ne '
      my $i = 0;
      while (/(\{"name":.*?\]\})/gs) {
        my $c = $1; $i++;
        my ($n)  = $c =~ /"name":"(.*?)"/;
        my ($g)  = $c =~ /"group":"(.*?)"/;
        my ($r)  = $c =~ /"raw":\[(.*?)\]/;
        my ($fl) = $c =~ /"frameLens":\[(.*?)\]/;
        my @t = split /,/, $r;
        my @f = split /,/, ($fl // "");
        printf "  %2d. %-20s %-18s %4d timings, %d part(s)\n",
               $i, $n, $g, scalar(@t), scalar(@f) || 1;
      }
      print "  (none)\n" unless $i;
    ' "$file"
    ;;

  *) usage ;;
esac
