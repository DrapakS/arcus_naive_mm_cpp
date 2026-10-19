#!/usr/bin/env bash
# The Python bot and the C++ one in paper mode side by side: the same config, the same live market,
# started together, each with its orders log. bench/compare_logs.py then sets the two logs against
# each other (what they placed, cancelled and filled, their round trips, guards, quotes, CPU and delays).
#
#   bash bench/paper_pair.sh start basis-both                    # a named pair (below): BTC both sides, basis pricing
#   bash bench/paper_pair.sh start config.btc-both.toml          # any config; name: btc-both
#   bash bench/paper_pair.sh start config.eth-both.toml eth       # name: eth
#   bash bench/paper_pair.sh status [name]
#   bash bench/paper_pair.sh stop [name]                          # SIGTERM to both, as Ctrl+C
#   bash bench/paper_pair.sh compare [name]                       # the comparison of the two logs
#
# Named pairs, as scripts/paper_ab.sh names its bots: basis (config.recommended.toml, BTC long, priced by the
# basis off Binance futures), basis-both (config.btc-both.toml, BTC both sides), ml (config.btc-ml.toml),
# ml-both (config.btc-ml-both.toml), ml-okx-only (config.btc-ml-okx-only.toml: BTC ML, the OKX guard only), eth-basis-both (config.eth-both.toml), eth-ml-both (config.eth-ml-both.toml),
# gld (config.gld.toml). Both bots read the very same file (this checkout's), so they run one config.
#
# Logs: paper-pair/<name>-py.log and <name>-cpp.log (+ .err). An earlier run's logs are kept under a name
# with the time they were moved aside. SETS: --set overrides for both, e.g. behind Binance's geo-block
#   SETS='binance.ws_url="wss://data-stream.binance.vision" binance.rest_url="https://data-api.binance.vision"'
# PY_REPO: the Python bot's checkout (default ../arcus_naive_mm); PY: its interpreter (default
# $PY_REPO/.venv/bin/python, else python3); BIN: the C++ binary (default build/naive_mm).
set -euo pipefail
cd "$(dirname "$0")/.."

PY_REPO=${PY_REPO:-../arcus_naive_mm}
if [ -z "${PY:-}" ]; then
    if [ -x "$PY_REPO/.venv/bin/python" ]; then PY=$(cd "$PY_REPO" && pwd)/.venv/bin/python; else PY=$(command -v python3); fi
fi
BIN=${BIN:-build/naive_mm}
OUT=paper-pair
PAIRS="basis:config.recommended.toml basis-both:config.btc-both.toml ml:config.btc-ml.toml ml-both:config.btc-ml-both.toml
       ml-okx-only:config.btc-ml-okx-only.toml
       eth-basis-both:config.eth-both.toml eth-ml-both:config.eth-ml-both.toml gld:config.gld.toml"
mkdir -p "$OUT"

cmd=${1:-status}
[ $# -gt 0 ] && shift

running() {  # running <name> <py|cpp>: the pid, if that bot is alive
    local pid
    pid=$(cat "$OUT/$1-$2.pid" 2>/dev/null) || return 1
    kill -0 "$pid" 2>/dev/null && echo "$pid"
}

names() {  # the runs named, or all there are
    if [ $# -gt 0 ]; then echo "$@"; else ls "$OUT"/*-py.log 2>/dev/null | sed 's|.*/||; s/-py\.log$//' | sort -u; fi
}

case "$cmd" in
start)
    config=${1:?"usage: bash bench/paper_pair.sh start PAIR|CONFIG [NAME]"}
    name=${2:-$(basename "$config" .toml | sed 's/^config\.//')}
    for pair in $PAIRS; do  # a named pair: its config, under its name
        if [ "${pair%%:*}" = "$config" ]; then name=${2:-$config}; config=${pair#*:}; fi
    done
    [ -f "$config" ] || { echo "no config $config (named pairs: $(for p in $PAIRS; do printf '%s ' "${p%%:*}"; done))" >&2; exit 2; }
    config=$(cd "$(dirname "$config")" && pwd)/$(basename "$config")
    sets=()
    for item in ${SETS:-}; do sets+=(--set "$item"); done
    [ -x "$BIN" ] || { echo "no C++ binary at $BIN: cmake -S . -B build && cmake --build build -j" >&2; exit 1; }
    "$PY" -c "import aiohttp, orjson, sortedcontainers" 2>/dev/null || { echo "$PY lacks the Python bot's requirements" >&2; exit 1; }
    "$BIN" --check --config "$config" ${sets[@]+"${sets[@]}"}
    (cd "$PY_REPO" && "$PY" -c "import sys; from naive_mm.config import load_config; load_config(sys.argv[1], sys.argv[2:])" \
        "$config" ${SETS:-})
    stamp=$(date +%Y%m%d-%H%M)
    for bot in py cpp; do
        if pid=$(running "$name" "$bot"); then echo "$name-$bot already running (pid $pid)" >&2; exit 1; fi
        for ext in log err; do
            if [ -s "$OUT/$name-$bot.$ext" ]; then mv "$OUT/$name-$bot.$ext" "$OUT/$name-$bot.$stamp.$ext"; fi
        done
    done
    log_dir=$(cd "$OUT" && pwd)
    (cd "$PY_REPO" && exec nohup "$PY" -m naive_mm --config "$config" ${sets[@]+"${sets[@]}"} --trade paper \
        --orders-log "$log_dir/$name-py.log" > /dev/null 2> "$log_dir/$name-py.err") &
    echo $! > "$OUT/$name-py.pid"
    nohup "$BIN" --config "$config" ${sets[@]+"${sets[@]}"} --trade paper --orders-log "$OUT/$name-cpp.log" \
        > /dev/null 2> "$OUT/$name-cpp.err" &
    echo $! > "$OUT/$name-cpp.pid"
    echo "$name: python pid $(cat "$OUT/$name-py.pid"), c++ pid $(cat "$OUT/$name-cpp.pid"); logs $OUT/$name-py.log and $OUT/$name-cpp.log"
    echo "config $config; python $(git -C "$PY_REPO" log -1 --format='%h %cs' 2>/dev/null || echo '?'), c++ $(git log -1 --format='%h %cs')"
    cores=$(nproc 2>/dev/null || echo 0)
    bots=$(pgrep -fc "naive_mm --config" || true)
    if [ "$cores" -gt 0 ] && [ "$bots" -gt "$cores" ]; then
        echo "WARNING: $bots bots on $cores CPU cores: they will stall each other, and the comparison measures the host" >&2
    fi
    sleep 5
    for bot in py cpp; do
        running "$name" "$bot" > /dev/null || { echo "$name-$bot stopped at once:" >&2; tail -5 "$OUT/$name-$bot.err" >&2; }
    done
    ;;
status)
    for name in $(names "$@"); do
        for bot in py cpp; do
            if pid=$(running "$name" "$bot"); then echo "== $name-$bot running (pid $pid)"; else echo "== $name-$bot not running"; fi
            tail -2 "$OUT/$name-$bot.err" 2>/dev/null | cut -c1-200 || true
        done
    done
    ;;
stop)
    for name in $(names "$@"); do
        for bot in py cpp; do
            if pid=$(running "$name" "$bot"); then kill -TERM "$pid" && echo "$name-$bot: stop sent (pid $pid)"; fi
            rm -f "$OUT/$name-$bot.pid"
        done
    done
    ;;
compare)
    for name in $(names "$@"); do
        "$PY" bench/compare_logs.py "$OUT/$name-py.log" "$OUT/$name-cpp.log"
    done
    ;;
*)
    echo "usage: bash bench/paper_pair.sh start PAIR|CONFIG [NAME] | status [NAME] | stop [NAME] | compare [NAME]" >&2
    exit 2
    ;;
esac
