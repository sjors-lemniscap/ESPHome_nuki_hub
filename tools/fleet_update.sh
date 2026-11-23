#!/bin/bash
# Builds and installs the firmware of several rooms over the air, one after the other.
#
#   tools/fleet_update.sh examples/fleet/entrance.yaml examples/fleet/room-u1.yaml ...
#
# The first file is the canary: it is updated first, and the others only follow if it comes
# back and answers. Any failure stops the run, so a bad build reaches one room at most.
# WAIT (default 90) is the time in seconds a room gets to come back after its update.
set -u
ESPHOME=${ESPHOME:-esphome}
WAIT=${WAIT:-90}
[ $# -ge 1 ] || { echo "usage: $0 <canary.yaml> [more.yaml ...]"; exit 2; }

for config in "$@"; do
  [ -f "$config" ] || { echo "$config not found"; exit 2; }
done

for config in "$@"; do
  echo "== $config: build"
  "$ESPHOME" compile "$config" > "${config%.yaml}.build.log" 2>&1 || { echo "build failed, see ${config%.yaml}.build.log"; exit 1; }
done

first=1
for config in "$@"; do
  echo "== $config: install"
  "$ESPHOME" upload "$config" > "${config%.yaml}.upload.log" 2>&1 || { echo "install failed, see ${config%.yaml}.upload.log"; exit 1; }
  echo "   waiting for the room to come back"
  deadline=$((SECONDS + WAIT)); back=0
  while [ $SECONDS -lt $deadline ]; do
    sleep 5
    # A second upload attempt is not wanted: only ask the device for its log for a moment
    if timeout 10 "$ESPHOME" logs "$config" --no-states > "${config%.yaml}.check.log" 2>&1; [ -s "${config%.yaml}.check.log" ] && grep -q "nuki_hub\|Connected\|esphome" "${config%.yaml}.check.log"; then
      back=1; break
    fi
  done
  [ $back -eq 1 ] || { echo "   did not come back within ${WAIT}s, stopping"; exit 1; }
  echo "   back"
  if [ $first -eq 1 ] && [ $# -gt 1 ]; then
    first=0
    echo "== canary done. Check the room (state, a keypad command), then press Enter to update the other $(( $# - 1 ))."
    read -r _
  fi
done
echo "all rooms updated"
