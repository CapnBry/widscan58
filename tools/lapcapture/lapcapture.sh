#!/bin/sh
# Capture one channel's raw levels and replay them through lap.c.
# GPL-3.0-or-later. Notes: NOTES.md (this directory).
#
#   lapcapture.sh capture usb|net SECONDS OUTDIR [capture.py options]
#   lapcapture.sh sweep   OUTDIR [ENTER/EXIT ...]   passes found per threshold pair
#   lapcapture.sh analyze OUTDIR ENTER EXIT [N]     replay at 0 and N (default 2, as the firmware), plot
#
# net: the board is on the RotorHazard host's USB port (RH_HOST, default
# rotorhaz.lan); the service is stopped and the capture runs there.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(dirname "$HERE")
REPO=$(dirname "$TOOLS")
RH_HOST=${RH_HOST:-rotorhaz.lan}
RH_PY=${RH_PY:-.venv/bin/python}   # relative to the remote home
RDIR=widscan58cap

build_replay() {
    gcc -O2 -I"$REPO/firmware/src/scan" -I"$REPO/firmware/src" -o "$1/replay" \
        "$HERE/replay.c" "$REPO/firmware/src/scan/lap.c" -lm
}

cmd=${1:-}; [ $# -gt 0 ] && shift
case "$cmd" in
capture)
    mode=$1 secs=$2 out=$3; shift 3
    mkdir -p "$out"
    case "$mode" in
    usb)
        python3 "$HERE/capture.py" -s "$secs" -o "$out/laps.csv" "$@" 2>&1 | tee "$out/capture.log"
        ;;
    net)
        ssh "$RH_HOST" "sudo -n systemctl stop rotorhazard; mkdir -p $RDIR"
        scp -q "$HERE/capture.py" "$TOOLS/widscan58cli.py" "$TOOLS/ttyfix.py" "$RH_HOST:$RDIR/"
        # Detached (stdin/out closed), so a dropped ssh does not stop the capture.
        ssh "$RH_HOST" "cd $RDIR && rm -f laps.csv && nohup ~/$RH_PY capture.py -s $secs -o laps.csv $* \
            </dev/null >capture.log 2>&1 &"
        sleep 4
        ssh "$RH_HOST" "cat $RDIR/capture.log"
        until ssh "$RH_HOST" "grep -q '^done\|Traceback\|failed' $RDIR/capture.log"; do
            sleep 5
            ssh "$RH_HOST" "tail -1 $RDIR/capture.log"
        done
        ssh "$RH_HOST" "tail -2 $RDIR/capture.log"
        scp -q "$RH_HOST:$RDIR/laps.csv" "$RH_HOST:$RDIR/capture.log" "$out/"
        ;;
    *) echo "mode: usb or net" >&2; exit 2 ;;
    esac
    ;;
sweep)
    out=$1; shift
    [ $# -gt 0 ] || set -- 145/120 135/115 125/105 120/100 115/95 110/90 105/85
    build_replay "$out"
    for th in "$@"; do
        n=$("$out/replay" "$out/laps.csv" "${th%/*}" "${th#*/}" median /dev/null 0 | grep -o '(pk' | wc -l)
        echo "$th: $n passes"
    done
    ;;
analyze)
    out=$1 enter=$2 exit_=$3 n=${4:-2}
    build_replay "$out"
    "$out/replay" "$out/laps.csv" "$enter" "$exit_" median "$out/trace.csv" 0 "$n" > "$out/rep.txt"
    python3 "$HERE/plot.py" "$out" --enter "$enter" --exit "$exit_" --n "$n" | tee "$out/passes.txt"
    echo "$out/timeline.png $out/pass_detail.png"
    ;;
*)
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
