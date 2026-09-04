#!/usr/bin/env bash
# Turns a backup export into an installable preset pack.
#
#   tools/make-preset.sh <backup.json> <out.json> <id> <brand> <model> <group>
#
# Preset packs are ordinary static files in data/presets/. The browser fetches
# one and posts each command to /api/import, which is the same path a restore
# takes -- so adding a device to the library needs no firmware change at all,
# only a new file and a line in index.json.
set -euo pipefail
[ $# -eq 6 ] || { sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
src=$1; out=$2; id=$3; brand=$4; model=$5; group=$6

BRAND="$brand" MODEL="$model" GROUP="$group" PID="$id" \
perl -0ne '
  my @cmds;
  while (/(\{"name":.*?\]\})/gs) { push @cmds, $1; }
  die "no commands found\n" unless @cmds;
  # Force every command into the pack group, so an install lands tidily.
  for (@cmds) { s/"group":"[^"]*"/"group":"$ENV{GROUP}"/ }
  printf qq({"id":"%s","brand":"%s","model":"%s","group":"%s","count":%d,"commands":[%s]}\n),
         $ENV{PID}, $ENV{BRAND}, $ENV{MODEL}, $ENV{GROUP}, scalar(@cmds), join(",", @cmds);
' "$src" > "$out"

printf 'wrote %s (%s bytes, %s commands)\n' \
  "$out" "$(wc -c < "$out" | tr -d ' ')" \
  "$(grep -o '"name":"' "$out" | wc -l | tr -d ' ')"
