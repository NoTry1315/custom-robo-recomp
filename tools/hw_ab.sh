#!/bin/sh
# Hardware A/B on the 2v2 replay, unattended, through cr_agent (tools/hw_loop.py run):
#   sh tools/hw_ab.sh <run N> <A.3dsx> <B.3dsx> <A tag> <B tag>
# Both builds need runtime/main.c's hw_return (any build of main since 9e688b9). The SD must already hold the 2v2
# save (data/saves/2026-10-22_pre-2v2) in /custom-robo/save/ and the replay flags cr_pad_script.txt,
# cr_pad_replay.txt and cr_dvdasync_off.txt (README "Hardware replay"); the agent never writes /custom-robo/save,
# so the script only checks it. Runs A, then B (logs/cr_autotest_<date>_hw-run<N>a|b-replay-2v2-<tag>.log), then
# prints the paired comparison (tools/hw_pair.py), route identity, perf counters and stalls. The 3DS stays in the
# agent. Each run takes ~5 minutes; run it in the background. A build goes to sdmc:/3ds/<its basename>: keep
# both named custom-robo-recomp.3dsx (in two folders) so they replace the SD build. Example:
#   sh tools/hw_ab.sh 42 /tmp/a/custom-robo-recomp.3dsx /tmp/b/custom-robo-recomp.3dsx main newthing
set -e
N=$1; A=$2; B=$3; TA=$4; TB=$5
[ -n "$TB" ] || { sed -n '2,9p' "$0"; exit 2; }
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1 PYTHONUTF8=1
D=$(date +%Y-%m-%d)
SAVE_MD5=$(md5sum < data/saves/2026-10-22_pre-2v2/CustomRobo-BattleRevolution.bin | cut -c1-32)
TMP=$(mktemp -d)
check_save() {
    python tools/hw_loop.py get /custom-robo/save/CustomRobo-BattleRevolution.bin "$(cygpath -m "$TMP" 2>/dev/null || echo "$TMP")/save.bin" >/dev/null
    [ "$(md5sum < "$TMP/save.bin" | cut -c1-32)" = "$SAVE_MD5" ] || { echo "SD save is not the 2v2 save ($1): stop"; exit 1; }
}
python tools/hw_loop.py ping
check_save "before A"
FL=$(python tools/hw_loop.py ls / | grep -c ' cr_\(pad_script\|pad_replay\|dvdasync_off\)\.txt$' || true)
[ "$FL" = 3 ] || { echo "replay flags not all active ($FL of 3): stop"; exit 1; }
LA=logs/cr_autotest_${D}_hw-run${N}a-replay-2v2-$TA.log
LB=logs/cr_autotest_${D}_hw-run${N}b-replay-2v2-$TB.log
python tools/hw_loop.py run --build "$A" --name ${D}_hw-run${N}a-replay-2v2-$TA --first 200 --timeout 600
check_save "after A"
python tools/hw_loop.py run --build "$B" --name ${D}_hw-run${N}b-replay-2v2-$TB --first 200 --timeout 600
python tools/hw_pair.py "$LA" "$LB"
python tools/citra/compare_logs.py "$LA" "$LB" | head -1
python tools/perf_avg.py "$LA" "$LB" | grep -E '^scene|^4\.2'
grep -h '^# stall' "$LA" "$LB" | cut -c1-120 || true
rm -rf "$TMP"
echo "A/B $N done"
