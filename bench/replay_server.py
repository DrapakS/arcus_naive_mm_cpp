"""A local exchange for the speed comparison of the bots (bench/compare.sh).

One process serves everything a BTC bot reads, from one random walk, at a set rate:
Binance spot (combined stream depth@100ms + bookTicker, REST depth), Binance spot
aggTrade, Binance futures bookTicker (under /public), and Arcus (l2OrderbookUpdates
deltas with lastSequenceId, public trades, REST l2OrderBook, markets, time). The
books follow the real protocols (update ids, snapshot splicing, absolute sizes), so a
bot syncs and prices as it would live; paper orders fill against the Arcus trades.

Exchange stamps (futures E, trade T) are the moment a frame is made, so the
"read ... after their event" figures of a bot's health line are the time the frame
spent in the server's and the bot's queues: how far behind the bot runs.

    python3 bench/replay_server.py --port 8765 --rate 1.0
    curl -s 127.0.0.1:8765/stats     # frames sent so far, per stream

Needs aiohttp (the Python bot's requirements have it). The rate scales every stream;
1.0 is a busy BTC minute (about 740 frames a second over all streams).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import random
import time

from aiohttp import WSMsgType, web

# frames per second of each stream at --rate 1 (a busy BTC minute, 15 Oct: ~860 frames/s over all feeds)
BASE = {"futures": 400.0, "spot": 150.0, "trades": 80.0, "arcus": 100.0, "arcus_trades": 3.0}
DEPTH_EVERY_S = 0.1  # Binance diff depth comes in 100 ms batches


def ms() -> int:
    return time.time_ns() // 1_000_000


class Side:
    """One side of a book on a price grid: price (in ticks) -> size."""

    def __init__(self) -> None:
        self.levels: dict[int, float] = {}


class Book:
    """A book on a grid of `tick`: bids strictly below `split`, asks from `split` up; moving the
    split moves the market a tick without ever crossing the book."""

    def __init__(self, rng: random.Random, mid: float, tick: float, decimals: int, depth: int) -> None:
        self.rng, self.tick, self.decimals, self.depth = rng, tick, decimals, depth
        self.split = round(mid / tick)
        self.bids = {self.split - 1 - i: self.size() for i in range(depth)}
        self.asks = {self.split + i: self.size() for i in range(depth)}

    def size(self) -> float:
        return round(self.rng.uniform(0.001, 3.0), 5)

    def px(self, ticks: int) -> str:
        return f"{ticks * self.tick:.{self.decimals}f}"

    def best(self) -> tuple[int, float, int, float]:
        b, a = max(self.bids), min(self.asks)
        return b, self.bids[b], a, self.asks[a]

    def step(self) -> tuple[list[list[str]], list[list[str]], bool]:
        """One change of the book: (bid changes, ask changes, whether the top changed)."""
        bids: list[list[str]] = []
        asks: list[list[str]] = []
        r = self.rng.random()
        if r < 0.05:  # the market moves a tick
            if self.rng.random() < 0.5:  # up: the lowest ask goes, a bid level appears there
                gone = min(self.asks)
                del self.asks[gone]
                asks.append([self.px(gone), "0"])
                self.bids[gone] = self.size()
                bids.append([self.px(gone), f"{self.bids[gone]:g}"])
                self.asks[max(self.asks) + 1] = self.size()
                asks.append([self.px(max(self.asks)), f"{self.asks[max(self.asks)]:g}"])
                low = min(self.bids)
                del self.bids[low]
                bids.append([self.px(low), "0"])
            else:  # down
                gone = max(self.bids)
                del self.bids[gone]
                bids.append([self.px(gone), "0"])
                self.asks[gone] = self.size()
                asks.append([self.px(gone), f"{self.asks[gone]:g}"])
                self.bids[min(self.bids) - 1] = self.size()
                bids.append([self.px(min(self.bids)), f"{self.bids[min(self.bids)]:g}"])
                high = max(self.asks)
                del self.asks[high]
                asks.append([self.px(high), "0"])
            return bids, asks, True
        # a size changes, near the top most often
        side, out = (self.bids, bids) if self.rng.random() < 0.5 else (self.asks, asks)
        ordered = sorted(side, reverse=side is self.bids)
        k = min(int(self.rng.expovariate(0.4)), len(ordered) - 1)
        price = ordered[k]
        side[price] = self.size()
        out.append([self.px(price), f"{side[price]:g}"])
        return bids, asks, k == 0

    def levels(self, n: int) -> tuple[list[list[str]], list[list[str]]]:
        bids = [[self.px(p), f"{self.bids[p]:g}"] for p in sorted(self.bids, reverse=True)[:n]]
        asks = [[self.px(p), f"{self.asks[p]:g}"] for p in sorted(self.asks)[:n]]
        return bids, asks


class Exchange:
    def __init__(self, rate: float, seed: int) -> None:
        self.rate = rate
        rng = random.Random(seed)
        self.rng = rng
        self.spot = Book(rng, 83000.0, 0.01, 2, 300)
        self.arcus = Book(rng, 82880.0, 0.1, 1, 150)
        self.spot_id = 1_000_000  # update id of the spot book
        self.arcus_seq = 5_000_000
        self.futures_id = 9_000_000
        self.trade_id = 3_000_000
        self.arcus_trade_id = 1
        self.futures_mid = 82950.0
        self.batch: dict[str, dict[str, str]] = {"b": {}, "a": {}}
        self.batch_first: int | None = None
        self.clients: dict[str, list[asyncio.Queue]] = {k: [] for k in ("spot", "trades", "futures", "arcus")}
        self.sent: dict[str, int] = {k: 0 for k in self.clients}

    def publish(self, stream: str, frame: str) -> None:
        for q in self.clients[stream]:
            q.put_nowait(frame)
            self.sent[stream] += 1

    # ------------------------------------------------------------ generators

    def spot_event(self) -> None:
        bids, asks, top = self.spot.step()
        self.spot_id += 1
        if self.batch_first is None:
            self.batch_first = self.spot_id
        for p, s in bids:
            self.batch["b"][p] = s
        for p, s in asks:
            self.batch["a"][p] = s
        if top:
            b, bs, a, as_ = self.spot.best()
            data = {"u": self.spot_id, "s": "BTCUSDT", "b": self.spot.px(b), "B": f"{bs:g}", "a": self.spot.px(a), "A": f"{as_:g}"}
            self.publish("spot", json.dumps({"stream": "btcusdt@bookTicker", "data": data}))

    def spot_depth(self) -> None:
        if self.batch_first is None:
            return
        data = {"e": "depthUpdate", "E": ms(), "s": "BTCUSDT", "U": self.batch_first, "u": self.spot_id,
                "b": [[p, s] for p, s in self.batch["b"].items()], "a": [[p, s] for p, s in self.batch["a"].items()]}
        self.batch, self.batch_first = {"b": {}, "a": {}}, None
        self.publish("spot", json.dumps({"stream": "btcusdt@depth@100ms", "data": data}))

    def trade_event(self) -> None:
        b, _, a, _ = self.spot.best()
        self.trade_id += 1
        price = self.spot.px(b if self.rng.random() < 0.5 else a)
        now = ms()
        data = {"e": "aggTrade", "E": now, "s": "BTCUSDT", "a": self.trade_id, "p": price, "q": "0.012", "f": self.trade_id,
                "l": self.trade_id, "T": now, "m": self.rng.random() < 0.5}
        self.publish("trades", json.dumps({"stream": "btcusdt@aggTrade", "data": data}))

    def futures_event(self) -> None:
        if self.rng.random() < 0.05:
            self.futures_mid += self.rng.choice((-0.1, 0.1))
        self.futures_id += self.rng.randint(1, 3)
        now = ms()
        data = {"e": "bookTicker", "u": self.futures_id, "s": "BTCUSDT", "b": f"{self.futures_mid - 0.05:.1f}",
                "B": f"{self.rng.uniform(0.1, 9):.3f}", "a": f"{self.futures_mid + 0.05:.1f}", "A": f"{self.rng.uniform(0.1, 9):.3f}",
                "T": now, "E": now}
        self.publish("futures", json.dumps({"stream": "btcusdt@bookTicker", "data": data}))

    def arcus_event(self) -> None:
        bids, asks, _ = self.arcus.step()
        self.arcus_seq += 1
        contents = {"bids": bids, "asks": asks, "lastSequenceId": self.arcus_seq, "globalSequenceId": self.arcus_seq}
        self.publish("arcus", json.dumps({"type": "channel_data", "channel": "l2OrderbookUpdates", "id": "BTC-USD", "contents": contents}))

    def arcus_trade(self) -> None:
        b, _, a, _ = self.arcus.best()
        buy = self.rng.random() < 0.5
        self.arcus_trade_id += 1
        trade = {"marketId": 1, "price": self.arcus.px(a if buy else b), "size": "0.0005", "side": "BUY" if buy else "SELL",
                 "timestamp": time.time_ns() // 1000, "tradeId": str(self.arcus_trade_id), "takerOrderId": f"t{self.arcus_trade_id}",
                 "makerOrderId": f"m{self.arcus_trade_id}", "takerAddress": "0x" + "ab" * 20, "makerAddress": "0x" + "cd" * 20}
        self.publish("arcus", json.dumps({"type": "channel_data", "channel": "trades", "id": "BTC-USD", "contents": [trade]}))

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        start = loop.time()
        done = {k: 0 for k in BASE}
        depth_done = 0
        events = {"futures": self.futures_event, "spot": self.spot_event, "trades": self.trade_event,
                  "arcus": self.arcus_event, "arcus_trades": self.arcus_trade}
        while True:
            elapsed = loop.time() - start
            for stream, per_s in BASE.items():
                due = int(elapsed * per_s * self.rate)
                for _ in range(due - done[stream]):
                    events[stream]()
                done[stream] = due
            depth_due = int(elapsed / DEPTH_EVERY_S)
            if depth_due > depth_done:
                self.spot_depth()
                depth_done = depth_due
            await asyncio.sleep(0.001)


async def sender(ws: web.WebSocketResponse, queue: asyncio.Queue) -> None:
    while not ws.closed:
        frame = await queue.get()
        await ws.send_str(frame)


async def stream(request: web.Request, kind: str) -> web.WebSocketResponse:
    ex: Exchange = request.app["exchange"]
    ws = web.WebSocketResponse(heartbeat=None)
    await ws.prepare(request)
    queue: asyncio.Queue = asyncio.Queue()
    task = asyncio.create_task(sender(ws, queue))
    subscribed = False
    if kind != "arcus":
        ex.clients[kind].append(queue)
        subscribed = True
    else:
        await ws.send_str(json.dumps({"type": "connected", "connection_id": "1"}))
    try:
        async for msg in ws:
            if msg.type is not WSMsgType.TEXT or kind != "arcus":
                continue
            data = json.loads(msg.data)
            if data.get("type") == "subscribe":
                queue.put_nowait(json.dumps({"type": "subscribed", "channel": data.get("channel"), "id": data.get("id"), "contents": {}}))
                if not subscribed:
                    ex.clients["arcus"].append(queue)
                    subscribed = True
    finally:
        for clients in ex.clients.values():
            if queue in clients:
                clients.remove(queue)
        task.cancel()
    return ws


async def spot_ws(request: web.Request) -> web.WebSocketResponse:
    return await stream(request, "trades" if "aggTrade" in request.query.get("streams", "") else "spot")


async def futures_ws(request: web.Request) -> web.WebSocketResponse:
    return await stream(request, "futures")


async def arcus_ws(request: web.Request) -> web.WebSocketResponse:
    return await stream(request, "arcus")


async def depth(request: web.Request) -> web.Response:
    ex: Exchange = request.app["exchange"]
    bids, asks = ex.spot.levels(int(request.query.get("limit", "1000")))
    return web.json_response({"lastUpdateId": ex.spot_id, "bids": bids, "asks": asks})


async def l2(request: web.Request) -> web.Response:
    ex: Exchange = request.app["exchange"]
    bids, asks = ex.arcus.levels(int(request.query.get("nLevels", "100")))
    return web.json_response({"bids": bids, "asks": asks, "lastSequenceId": ex.arcus_seq, "timestamp": time.time_ns() // 1000})


async def markets(request: web.Request) -> web.Response:
    return web.json_response({"markets": [{
        "marketId": 1, "marketDisplayName": "BTC-USD", "status": "ONLINE", "tickSize": "0.1", "stepSize": "0.00000001",
        "minOrderSize": "0.0001", "minOrderNotional": "5", "maxOrderSize": "10000",
    }]})


async def server_time(request: web.Request) -> web.Response:
    return web.json_response({"timeNs": time.time_ns()})


async def stats(request: web.Request) -> web.Response:
    ex: Exchange = request.app["exchange"]
    return web.json_response({"sent": ex.sent, "total": sum(ex.sent.values()), "rate": ex.rate})


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rate", type=float, default=1.0, help="every stream's rate times this (1.0: a busy BTC minute)")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args()
    app = web.Application()
    app["exchange"] = exchange = Exchange(args.rate, args.seed)
    app.router.add_get("/stream", spot_ws)
    app.router.add_get("/public/stream", futures_ws)
    app.router.add_get("/v1/ws", arcus_ws)
    app.router.add_get("/api/v3/depth", depth)
    app.router.add_get("/v1/l2OrderBook/{market}", l2)
    app.router.add_get("/v1/markets", markets)
    app.router.add_get("/v1/time", server_time)
    app.router.add_get("/stats", stats)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, "127.0.0.1", args.port).start()
    print(f"replay server on 127.0.0.1:{args.port}, rate x{args.rate:g}", flush=True)
    await exchange.run()


if __name__ == "__main__":
    try:
        import uvloop

        uvloop.install()
    except ImportError:
        pass
    asyncio.run(main())
