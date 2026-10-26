#!/bin/sh
# Snapshot of the running game on the 3DS through Rosalina's debugger (Rosalina menu -> Debugger
# options -> Enable debugger). Attaching pauses the game; it resumes at detach.
#   sh tools/hw_dump.sh [ip] [tag]        (Git Bash; default ip 192.168.1.129)
# Writes logs/hwdump/<date>_<tag>/: processes.txt, regions.txt (monitor getmemregions), threads.txt
# (registers + backtraces of every thread), mem_<start>.bin per readable region. The guest's 24 MB RAM
# is the static ram[] inside one of the regions: find it offline by main.dol's bytes (section 0 is at
# guest 0x80003100 = ram + 0x3100).
IP=${1:-192.168.1.129}
TAG=${2:-snap}
REPO=$(cd "$(dirname "$0")/.." && pwd -W 2>/dev/null || pwd)
REL=logs/hwdump/$(date +%Y-%m-%d_%H%M%S)_$TAG
OUT="$REPO/$REL"
mkdir -p "$OUT"
GDB=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb
run() { MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO:/proj" -w "/proj/$REL" mkw3ds-build:2026-10-09 "$@"; }

run $GDB -q -batch -ex "set remotetimeout 20" -ex "target extended-remote $IP:4000" -ex "info os processes" \
    -ex disconnect > "$OUT/processes.txt" 2>&1
PID=$(awk '$2 == "3dsx_app" { print $1; exit }' "$OUT/processes.txt")
[ -n "$PID" ] || { echo "no 3dsx_app process (see $OUT/processes.txt)"; exit 1; }
echo "game pid $PID, writing $REL"

# readable regions from "monitor getmemregions" -> one dump command each (largest skipped above 256 MB)
cat > "$OUT/gen.sh" <<'EOF'
grep -oiE '0x[0-9a-f]+ *- *0x[0-9a-f]+.*' regions.txt | while read -r a dash b rest; do
  case "$rest" in ---*) continue ;; esac
  s=$((a)); e=$((b))
  [ $((e & 0xfff)) -eq $((0xfff)) ] && e=$((e + 1))   # inclusive end -> exclusive
  [ $((e - s)) -gt 268435456 ] && continue
  printf 'dump binary memory mem_%08x.bin 0x%08x 0x%08x\n' $s $s $e
done
EOF
cat > "$OUT/snap.gdb" <<EOF
set pagination off
set confirm off
set remotetimeout 30
set width 0
target extended-remote $IP:4000
attach $PID
set logging overwrite on
set logging redirect on
set logging file regions.txt
set logging enabled on
monitor getmemregions
set logging enabled off
set logging file threads.txt
set logging enabled on
info threads
thread apply all info registers
thread apply all bt 30
set logging enabled off
shell sh gen.sh > dump.gdb
source dump.gdb
detach
EOF
time run $GDB -q -batch -x snap.gdb > "$OUT/gdb.txt" 2>&1
echo "done: $(ls "$OUT"/mem_*.bin 2>/dev/null | wc -l) regions dumped; gdb output in $REL/gdb.txt"
