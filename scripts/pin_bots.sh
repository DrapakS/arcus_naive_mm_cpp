#!/usr/bin/env bash
# Each running live bot on a CPU core of its own, at a higher priority: on a busy moment every bot wakes
# at once, and with more of them than cores one waits for the others (loop stalls of 100-300 ms, 19 Oct:
# 4 bots on 4 cores, ~90 stalls an hour each). Changes no setting of the bots, only where the OS runs them.
#
#   sudo bash scripts/pin_bots.sh            pin and renice every running "naive_mm --config" process
#   bash scripts/pin_bots.sh show            where each one runs now (no sudo needed)
#
# A bot goes to the core its config names below (ETH ML on the last core: the busiest; the OS's own work
# goes to core 0, with the lightest bot); one not listed takes the next free core. Run it again after a
# bot restarts: a new process starts on all cores at the default priority.
# NICE: -5 for the bots (the OS's daemons stay at 0); paper bots (--trade paper) get +10 and share the cores.
set -euo pipefail

declare -A CORE=(
    [config.eth-ml-both.toml]=3
    [config.btc-both.toml]=2
    [config.btc-ml-okx-only.toml]=2   # the BTC bot either way: one of the two runs live
    [config.gld-both.toml]=1
    [config.nvda-both.toml]=0
)
NICE=${NICE:--5}
cores=$(nproc)

pids=$(pgrep -f "naive_mm --config" || true)
if [ -z "$pids" ]; then echo "no bot running (pgrep -f 'naive_mm --config')"; exit 0; fi

if [ "${1:-}" = show ]; then
    for pid in $pids; do
        printf "%-7s core(s) %-8s nice %3s  %s\n" "$pid" "$(taskset -pc "$pid" | sed 's/.*: //')" \
            "$(ps -o ni= -p "$pid" | tr -d ' ')" "$(ps -o args= -p "$pid" | cut -c1-110)"
    done
    exit 0
fi
if [ "$(id -u)" -ne 0 ]; then echo "run with sudo (a negative nice and pinning other users' processes need root)" >&2; exit 1; fi

used=()
next=0
for pid in $pids; do
    args=$(ps -o args= -p "$pid")
    config=$(sed -E 's/.*--config ([^ ]+).*/\1/' <<< "$args")
    if [[ "$args" == *"--trade paper"* ]]; then  # paper: no core of its own, below the live bots
        renice -n 10 -p "$pid" > /dev/null
        echo "$pid paper ($(basename "$config")): nice 10, all cores"
        continue
    fi
    core=${CORE[$(basename "$config")]:-}
    if [ -z "$core" ]; then
        while [[ " ${used[*]} ${CORE[*]} " == *" $next "* ]] && [ "$next" -lt "$cores" ]; do next=$((next + 1)); done
        core=$((next % cores))
    fi
    used+=("$core")
    taskset -acp "$core" "$pid" > /dev/null  # -a: every thread (the log writers and the stall catcher too)
    renice -n "$NICE" -p "$pid" > /dev/null
    echo "$pid $(basename "$config"): core $core, nice $NICE"
done
