#!/bin/sh
# Run on the intersections QNX node. Displays and input attach separately.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=${LOCAL_BINARY:-"$base/build/x86_64-debug/local_intersection_controller"}
mode=${1:--l}
case "$mode" in -l|-g) ;; *) echo "Usage: sh start_locals.sh [-l|-g]" >&2; exit 1;; esac
pids=""
cleanup() {
    trap - EXIT INT TERM
    for pid in $pids; do kill "$pid" 2>/dev/null || :; done
    wait || :
}
trap cleanup EXIT
trap 'exit 0' INT TERM
for id in I1 I2 I3 I4 I5 I6; do
    "$binary" "$mode" -i "$id" --role core &
    pids="$pids $!"
    "$binary" "$mode" -i "$id" --role comm &
    pids="$pids $!"
done
echo "Six core and six communication processes started. Ctrl-C stops this group."
wait
