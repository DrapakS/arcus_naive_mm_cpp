#!/usr/bin/env bash
# Speed of the C++ bot against the Python one on the same market data: each runs in paper
# mode against bench/replay_server.py (a local exchange streaming a busy BTC market, scaled
# by RATE), one bot at a time, with the same config. After a warm-up the script measures
# the bot's CPU time (all its threads) over DURATION seconds against the frames the server
# sent it, its memory, and the health line's delays (how long frames waited before the bot
# read them).
#
#   bash bench/compare.sh                         # both bots, rates 1 and 4
#   RATES="1 4 16" DURATION=60 bash bench/compare.sh
#   BOTS=cpp CONFIG=config.btc-ml-both.toml bash bench/compare.sh
#   EXTRA="agent.tob_lines=false" bash bench/compare.sh     # more --set overrides for both
#
# PY_REPO: the Python bot's checkout (default ../arcus_naive_mm), PY: its interpreter (default
# python3; needs its requirements: aiohttp, orjson, sortedcontainers, uvloop, PyNaCl), BIN: the
# C++ binary (default build/naive_mm). The server runs on the same interpreter.
set -euo pipefail
cd "$(dirname "$0")/.."

PY=${PY:-python3}
PY_REPO=${PY_REPO:-../arcus_naive_mm}
BIN=${BIN:-build/naive_mm}
CONFIG=${CONFIG:-config.btc-both.toml}
RATES=${RATES:-1 4}
BOTS=${BOTS:-python cpp}
WARMUP=${WARMUP:-20}
DURATION=${DURATION:-60}
PORT=${PORT:-8765}
OUT=${OUT:-bench/out}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

server_pid=""
bot_pid=""
cleanup() {
    [ -n "$bot_pid" ] && kill -INT "$bot_pid" 2>/dev/null || true
    [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT

cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat"; }
rss_kb() { awk '/VmRSS/ {print $2}' "/proc/$1/status"; }
frames_sent() { curl -s "127.0.0.1:$PORT/stats" | "$PY" -c "import json, sys; print(json.load(sys.stdin)['total'])"; }

sets=(
    --set "binance.ws_url=\"ws://127.0.0.1:$PORT\"" --set "binance.rest_url=\"http://127.0.0.1:$PORT\""
    --set "futures.ws_url=\"ws://127.0.0.1:$PORT\""
    --set "arcus.ws_url=\"ws://127.0.0.1:$PORT/v1/ws\"" --set "arcus.rest_url=\"http://127.0.0.1:$PORT\""
    --set "bybit.enabled=false" --set "agent.health_interval_s=10" --set "lag.max_ms=0"
)
# EXTRA: more overrides for both bots, e.g. EXTRA="agent.tob_lines=false trading.mode=quote"
for item in ${EXTRA:-}; do sets+=(--set "$item"); done
hz=$(getconf CLK_TCK)

printf "%-7s %5s %9s %9s %10s %8s %9s   %s\n" bot rate frames/s cpu% "us/frame" "rss MB" "loop p99" "futures tickers read (ms after the stamp)"
for rate in $RATES; do
    for bot in $BOTS; do
        "$PY" bench/replay_server.py --port "$PORT" --rate "$rate" > "$OUT/server.log" 2>&1 &
        server_pid=$!
        sleep 1.5
        log="$OUT/$bot-x$rate.log"
        rm -f "$log"
        if [ "$bot" = python ]; then
            (cd "$PY_REPO" && exec "$PY" -m naive_mm --config "$CONFIG" "${sets[@]}" --trade paper --orders-log "$log") \
                > /dev/null 2> "$OUT/$bot-x$rate.err" &
        else
            "$BIN" --config "$CONFIG" "${sets[@]}" --trade paper --orders-log "$log" > /dev/null 2> "$OUT/$bot-x$rate.err" &
        fi
        bot_pid=$!
        sleep "$WARMUP"
        c0=$(cpu_ticks "$bot_pid") f0=$(frames_sent)
        sleep "$DURATION"
        c1=$(cpu_ticks "$bot_pid") f1=$(frames_sent) rss=$(rss_kb "$bot_pid")
        kill -INT "$bot_pid"; wait "$bot_pid" 2>/dev/null || true; bot_pid=""
        kill "$server_pid"; wait "$server_pid" 2>/dev/null || true; server_pid=""
        cpu_s=$(echo "($c1 - $c0) / $hz" | bc -l)
        frames=$((f1 - f0))
        health=$(grep -- "--- health over" "$log" | tail -1)
        loop=$(echo "$health" | sed -n 's/.*event loop lag p50 [^/]*\/ p99 \([^ ]*\) .*/\1/p')
        futures=$(echo "$health" | sed -n 's/.*Binance futures tickers read \(p50 [^(]*\).*/\1/p')
        printf "%-7s %5s %9.0f %8.1f%% %10.1f %8.1f %9s   %s\n" "$bot" "x$rate" "$(echo "$frames / $DURATION" | bc -l)" \
            "$(echo "100 * $cpu_s / $DURATION" | bc -l)" "$(echo "1000000 * $cpu_s / $frames" | bc -l)" "$(echo "$rss / 1024" | bc -l)" \
            "${loop:-?}" "${futures:-?}"
        sleep 1
    done
done
