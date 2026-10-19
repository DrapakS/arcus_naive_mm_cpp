#!/usr/bin/env bash
# Paper A/B on BTC: the basis pricing (config.recommended.toml) next to the ML model (config.btc-ml.toml);
# and gold, GLD-USD (config.gld.toml). No keys
# needed: paper orders live in the bot only and never reach the exchange.
#
#   bash scripts/paper_ab.sh                 start basis and ml (logs: paper-basis.log / .err, paper-ml.log / .err)
#   bash scripts/paper_ab.sh start ml        start only the linear one (paper-ml.log / .err); any names:
#                                            basis, ml, gld; two-sided: basis-both, ml-both, and
#                                            basis-both-nodrift (config.btc-both.toml with the drift markup off);
#                                            ETH two-sided: eth-basis-both (config.eth-both.toml), eth-ml-both
#                                            (config.eth-ml-both.toml)
#   bash scripts/paper_ab.sh start basis-both ml-both            BTC: basis vs ML, both sides
#   bash scripts/paper_ab.sh start eth-basis-both eth-ml-both    ETH: the same A/B
#   bash scripts/paper_ab.sh start basis-both basis-both-nodrift    the drift guard A/B
#   bash scripts/paper_ab.sh start eth-ml-both eth-ml-okx-only   ETH ML as live vs the same with every guard off but OKX's
#                                            (Binance spot/futures/trades, Bybit, Arcus, trend, add, lag, takes guards and
#                                            the drift markup off; the Bybit feed stays, the model reads it)
#   bash scripts/paper_ab.sh start ml-both btc-ml-okx-only       BTC the same: config.btc-ml-both.toml (the guards of the
#                                            live BTC bot) vs the same with every guard off but an OKX one (BTC-USDT-SWAP,
#                                            200 ms / 0.2 bp / 300 ms, one connection: the live BTC bot has no OKX guard)
# Logs of an earlier run are kept: renamed with the time they were moved aside (paper-basis.20261016-2030.log).
# Each bot wants a CPU core of its own: with more bots than cores (live ones included) it warns, as the event
# loops then stall each other and the comparison measures the host rather than the pricing.
#   bash scripts/paper_ab.sh status [names]  are they running, and the last lines of their logs (all by default)
#   bash scripts/paper_ab.sh stop [names]    stop them (SIGTERM, as Ctrl+C; all by default)
#
# The binary: $BIN if set, else build/naive_mm (cmake -S . -B build && cmake --build build).
set -euo pipefail
cd "$(dirname "$0")/.."

ALL="basis:config.recommended.toml ml:config.btc-ml.toml gld:config.gld.toml
     basis-both:config.btc-both.toml ml-both:config.btc-ml-both.toml
     basis-both-nodrift:config.btc-both.toml@drift.markup_factor=1
     eth-basis-both:config.eth-both.toml eth-ml-both:config.eth-ml-both.toml
     eth-ml-okx-only:config.eth-ml-both.toml@guard.drawdown=0@futures.drawdown=0@trades.drawdown=0@bybit.drawdown=0@arcus_guard.drawdown=0@trend.drawdown=0@add_guard.drawdown=0@lag.max_ms=0@takes.pause_ms=0@drift.markup_factor=1
     btc-ml-okx-only:config.btc-ml-both.toml@guard.drawdown=0@futures.drawdown=0@trades.drawdown=0@bybit.drawdown=0@arcus_guard.drawdown=0@trend.drawdown=0@add_guard.drawdown=0@lag.max_ms=0@takes.pause_ms=0@drift.markup_factor=1@okx.enabled=true@okx.connections=1"
cmd=${1:-start}
[ $# -gt 0 ] && shift
names="$*"
if [ -z "$names" ]; then
    if [ "$cmd" = start ]; then names="basis ml"; else names="basis ml gld basis-both ml-both basis-both-nodrift eth-basis-both eth-ml-both eth-ml-okx-only btc-ml-okx-only"; fi
fi
RUNS=""
for name in $names; do
    match=""
    for run in $ALL; do
        if [ "${run%%:*}" = "$name" ]; then match=$run; fi
    done
    if [ -z "$match" ]; then
        echo "unknown run $name: basis, ml, gld, basis-both, ml-both, basis-both-nodrift, eth-basis-both, eth-ml-both, eth-ml-okx-only or btc-ml-okx-only" >&2; exit 2
    fi
    RUNS="$RUNS $match"
done

running() {  # running <name>: the pid, if that run is alive
    local pid
    pid=$(cat "paper-$1.pid" 2>/dev/null) || return 1
    kill -0 "$pid" 2>/dev/null && echo "$pid"
}

case "$cmd" in
start)
    BIN=${BIN:-build/naive_mm}
    if [ ! -x "$BIN" ]; then
        echo "no bot binary at $BIN: build it first (cmake -S . -B build && cmake --build build -j)," >&2
        echo "or point BIN at the one the live bots run with: BIN=/path/to/naive_mm bash scripts/paper_ab.sh" >&2
        exit 1
    fi
    stamp=$(date +%Y%m%d-%H%M)
    new=0
    for run in $RUNS; do
        name=${run%%:*} config=${run#*:}
        sets=()  # name:config@section.key=value[@...]: that config with each value overridden (--set)
        if [ "${config#*@}" != "$config" ]; then
            IFS=@ read -r -a overrides <<< "${config#*@}"
            for o in "${overrides[@]}"; do sets+=(--set "$o"); done
            config=${config%%@*}
        fi
        if pid=$(running "$name"); then
            echo "paper-$name already running (pid $pid)"
            continue
        fi
        if ! "$BIN" --check --config "$config" ${sets[@]+"${sets[@]}"}; then
            echo "paper-$name: $config does not load (above), not started" >&2
            continue
        fi
        for ext in log err; do  # an earlier run's logs: kept under a name of their own
            if [ -s "paper-$name.$ext" ]; then mv "paper-$name.$ext" "paper-$name.$stamp.$ext"; fi
        done
        new=$((new + 1))
        nohup "$BIN" --config "$config" ${sets[@]+"${sets[@]}"} --trade paper --orders-log "paper-$name.log" \
            > /dev/null 2> "paper-$name.err" &
        echo $! > "paper-$name.pid"
        echo "paper-$name started: pid $!, $config ${sets[*]+${sets[*]}}, logs paper-$name.log and paper-$name.err ($BIN)"
    done
    bots=$(pgrep -fc "naive_mm --config" || true)
    cores=$(nproc 2>/dev/null || echo 0)
    if [ "$new" -gt 0 ] && [ "$cores" -gt 0 ] && [ "$bots" -gt "$cores" ]; then
        echo "WARNING: $bots bots (paper and live) on $cores CPU cores: their event loops will stall each other." >&2
    fi
    sleep 3
    for run in $RUNS; do
        name=${run%%:*}
        running "$name" > /dev/null || { echo "paper-$name stopped at once:" >&2; tail -5 "paper-$name.err" >&2; }
    done
    ;;
status)
    for run in $RUNS; do
        name=${run%%:*}
        if pid=$(running "$name"); then echo "== paper-$name running (pid $pid)"; else echo "== paper-$name not running"; fi
        tail -3 "paper-$name.err" 2>/dev/null || true
    done
    ;;
stop)
    for run in $RUNS; do
        name=${run%%:*}
        if pid=$(running "$name"); then kill -TERM "$pid" && echo "paper-$name: stop sent (pid $pid)"; fi
        rm -f "paper-$name.pid"
    done
    ;;
*)
    echo "usage: bash scripts/paper_ab.sh [start|status|stop] [basis ml gld basis-both ml-both eth-basis-both eth-ml-both eth-ml-okx-only]" >&2
    exit 2
    ;;
esac
