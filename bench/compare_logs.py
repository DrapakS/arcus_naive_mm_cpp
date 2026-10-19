"""Set the orders logs of two paper runs against each other: the Python bot and the C++ one
started side by side on the same market (bench/paper_pair.sh), or any two runs.

Both bots write the same lines to their orders log (--orders-log): the placements, cancels,
fills and final states of the orders, the round trips, the guards' trips and pauses, the
buy/sell price lines and, once a minute, the health line (event loop lag, CPU share, how late
the exchanges' messages were read). This reads both logs over the time they overlap and prints,
side by side:

- the orders: placements, cancels by reason, fills, final states, rejects;
- the round trips: how many, their result in $ and bp, the fills in them;
- the guards: DROP / RISE / TAKE / LATE / DRIFT / FALL / RALLY lines;
- the quotes: how often the two bots' buy and sell prices were the same, second by second,
  and how far apart they were otherwise; the same for the fair bid;
- the fills matched between the two (the same side and price within a second);
- the health lines: CPU share of a core, event loop lag, read delays (medians over the run).

    python3 bench/compare_logs.py paper-pair/btc-both-py.log paper-pair/btc-both-cpp.log
    python3 bench/compare_logs.py A.log B.log --names python c++ --from 07:10 --to 08:10

Only the standard library is needed.
"""

from __future__ import annotations

import argparse
import re
import statistics
from bisect import bisect_right
from collections import Counter
from dataclasses import dataclass, field

LINE = re.compile(r"^(\d\d):(\d\d):(\d\d)\.(\d{3}) (\w+)\s+(\S+): (.*)$")
NUMBER = re.compile(r"[-+]?\d+(?:\.\d+)?(?:e[-+]?\d+)?")
TRIP = re.compile(r"held ([\d.]+) s, ([-+][\d.]+) bp \(\$([-+][\d.]+)\); worst .*?; (\d+) fills")
HEALTH_PARTS = {
    "loop p50": re.compile(r"event loop lag p50 ([\d.]+)"),
    "loop p99": re.compile(r"event loop lag p50 [\d.]+ / p99 ([\d.]+)"),
    "loop max": re.compile(r"event loop lag p50 [\d.]+ / p99 [\d.]+ / max ([\d.]+)"),
    "cpu %": re.compile(r"cpu ([\d.]+)% of a core"),
    "futures read p50": re.compile(r"Binance futures tickers read p50 ([\d.]+)"),
    "futures read p99": re.compile(r"Binance futures tickers read p50 [\d.]+ / p99 ([\d.]+)"),
    "spot trades read p50": re.compile(r"Binance spot trades read p50 ([\d.]+)"),
    "spot trades read p99": re.compile(r"Binance spot trades read p50 [\d.]+ / p99 ([\d.]+)"),
    "order events read p50": re.compile(r"Arcus order events read p50 ([\d.]+)"),
}
GUARDS = ("DROP", "RISE", "TAKE", "LATE", "DRIFT", "FALL", "RALLY")


@dataclass
class Run:
    path: str
    version: str = ""
    config: str = ""
    start: float | None = None
    end: float | None = None
    places: Counter = field(default_factory=Counter)  # by side
    cancels: Counter = field(default_factory=Counter)  # by reason
    finals: Counter = field(default_factory=Counter)  # CANCELED / FILLED / REJECTED
    fills: list[tuple[float, str, float, float]] = field(default_factory=list)  # (t, side, size, price)
    trips: list[tuple[float, float, float, int]] = field(default_factory=list)  # (held s, bp, $, fills)
    guards: Counter = field(default_factory=Counter)
    quotes: list[tuple[float, float | None, float | None, float | None]] = field(default_factory=list)  # (t, buy, sell, fair)
    health: dict[str, list[float]] = field(default_factory=dict)
    at_exit: str = ""
    lines: int = 0


def seconds(m: re.Match) -> float:
    h, mi, s, ms = (int(m[i]) for i in range(1, 5))
    return h * 3600 + mi * 60 + s + ms / 1000


def cancel_reason(message: str) -> str:
    """The reason of a ">>> CANCEL" line, numbers taken out, so cancels can be counted by kind."""
    text = message.split("; sent on ")[0]
    if "]: " in text:
        text = text.split("]: ", 1)[1]
    elif "): " in text:
        text = text.split("): ", 1)[1]
    else:
        return "?"
    for prefix in ("drop: ", "rise: ", "take: ", "late data: "):
        if text.startswith(prefix):
            rest = text[len(prefix):]
            words = []
            for word in rest.split():  # the guard's label: the words before the first number
                if NUMBER.fullmatch(word.strip("$,")):
                    break
                words.append(word)
            return prefix + " ".join(words[:3])
    if text.startswith("the book came to it"):
        return "the book came to it"
    return NUMBER.sub("#", text)[:60]


def number_after(text: str, key: str) -> float | None:
    m = re.search(re.escape(key) + r" ([-\d.]+)", text)
    return float(m[1]) if m and m[1] not in ("-", ".") else None


def parse(path: str, start: float | None, end: float | None) -> Run:
    run = Run(path)
    day = 0.0
    last = None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            m = LINE.match(raw.rstrip("\n"))
            if m is None:
                continue
            t = seconds(m)
            if last is not None and t + day < last - 12 * 3600:  # past midnight
                day += 86_400
            t += day
            last = t
            logger, message = m[6], m[7]
            if logger == "naive_mm.config" and message.startswith("naive_mm ") and not run.version:
                run.version = message[len("naive_mm "):]
                continue
            if (start is not None and t < start) or (end is not None and t > end):
                continue
            run.start = t if run.start is None else run.start
            run.end = t
            run.lines += 1
            if logger == "naive_mm.quotes":
                buy, sell = number_after(message, "buy"), number_after(message, "sell")
                run.quotes.append((t, buy, sell, number_after(message, "fair bid")))
            elif message.startswith(">>> PLACE "):
                run.places[message.split()[2]] += 1
            elif message.startswith(">>> CANCEL "):
                run.cancels[cancel_reason(message)] += 1
            elif message.startswith("<<< FILL "):
                parts = message.split()
                side, size = parts[2], float(parts[3])
                price = float(parts[5]) if len(parts) > 5 and parts[4] == "@" else float("nan")
                run.fills.append((t, side, size, price))
            elif message.startswith(("<<< CANCELED ", "<<< FILLED ", "<<< REJECTED ")):
                run.finals[message.split()[1]] += 1
            elif message.startswith("=== ROUND TRIP"):
                if (trip := TRIP.search(message)) is not None:
                    run.trips.append((float(trip[1]), float(trip[2]), float(trip[3]), int(trip[4])))
            elif message.startswith("=== AT EXIT"):
                run.at_exit = message
            elif message.startswith("!!! "):
                word = message.split()[1]
                if word in GUARDS:
                    run.guards[word] += 1
            elif message.startswith("--- health over"):
                for name, pattern in HEALTH_PARTS.items():
                    if (h := pattern.search(message)) is not None:
                        run.health.setdefault(name, []).append(float(h[1]))
    return run


def at(series: list[tuple], times: list[float], t: float) -> tuple | None:
    """The latest entry of a time series as of t (`times`: its entries' times)."""
    i = bisect_right(times, t) - 1
    return series[i] if i >= 0 else None


def hms(t: float | None) -> str:
    if t is None:
        return "-"
    t %= 86_400
    return f"{int(t // 3600):02d}:{int(t % 3600 // 60):02d}:{t % 60:06.3f}"


def row(label: str, a, b, width: int = 34) -> str:
    def cell(v) -> str:
        if isinstance(v, float):
            return f"{v:.4g}" if abs(v) < 1000 else f"{v:.0f}"
        return str(v)
    return f"  {label:<{width}} {cell(a):>16} {cell(b):>16}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--names", nargs=2, default=("python", "c++"))
    parser.add_argument("--from", dest="start", help="HH:MM[:SS] (time of day in the logs)")
    parser.add_argument("--to", dest="end", help="HH:MM[:SS]")
    args = parser.parse_args()

    def tod(text: str | None) -> float | None:
        if text is None:
            return None
        parts = [float(x) for x in text.split(":")]
        return parts[0] * 3600 + parts[1] * 60 + (parts[2] if len(parts) > 2 else 0)

    a0, b0 = parse(args.a, tod(args.start), tod(args.end)), parse(args.b, tod(args.start), tod(args.end))
    if a0.start is None or b0.start is None:
        raise SystemExit("a log has no lines in that time")
    # the time both ran: the comparison is over that only
    start, end = max(a0.start, b0.start), min(a0.end, b0.end)
    if end <= start:
        raise SystemExit("the two logs do not overlap in time")
    a, b = parse(args.a, start, end), parse(args.b, start, end)
    na, nb = args.names
    minutes = (end - start) / 60
    print(f"{na}: {a.path}\n  {a.version}\n{nb}: {b.path}\n  {b.version}")
    print(f"common time {hms(start)} .. {hms(end)} ({minutes:.1f} min)\n")
    print(f"  {'':<34} {na:>16} {nb:>16}")

    print("orders")
    for side in ("BUY", "SELL"):
        print(row(f"placed {side.lower()}s", a.places[side], b.places[side]))
    print(row("placements a minute", sum(a.places.values()) / minutes, sum(b.places.values()) / minutes))
    print(row("cancels", sum(a.cancels.values()), sum(b.cancels.values())))
    for reason, _ in (a.cancels + b.cancels).most_common(12):
        print(row(f"  {reason[:32]}", a.cancels[reason], b.cancels[reason]))
    for final in ("CANCELED", "FILLED", "REJECTED"):
        print(row(final.lower(), a.finals[final], b.finals[final]))
    print(row("fills", len(a.fills), len(b.fills)))
    print(row("filled size", sum(f[2] for f in a.fills), sum(f[2] for f in b.fills)))

    print("round trips")
    print(row("trips", len(a.trips), len(b.trips)))
    print(row("result $", sum(t[2] for t in a.trips), sum(t[2] for t in b.trips)))
    print(row("mean bp", statistics.fmean(t[1] for t in a.trips) if a.trips else "-",
              statistics.fmean(t[1] for t in b.trips) if b.trips else "-"))
    print(row("median held s", statistics.median(t[0] for t in a.trips) if a.trips else "-",
              statistics.median(t[0] for t in b.trips) if b.trips else "-"))
    if a.at_exit or b.at_exit:
        print(f"  at exit: {na}: {a.at_exit or '-'}\n           {nb}: {b.at_exit or '-'}")

    print("guards (!!! lines)")
    for word in GUARDS:
        if a.guards[word] or b.guards[word]:
            print(row(word, a.guards[word], b.guards[word]))

    print("quotes (the price lines, sampled every second)")
    same_buy = same_sell = both = 0
    diffs_buy, diffs_sell, diffs_fair = [], [], []
    times_a, times_b = [q[0] for q in a.quotes], [q[0] for q in b.quotes]
    t = start + 1.0
    while t <= end:
        qa, qb = at(a.quotes, times_a, t), at(b.quotes, times_b, t)
        if qa is not None and qb is not None:
            both += 1
            for i, same, diffs in ((1, "buy", diffs_buy), (2, "sell", diffs_sell), (3, "fair", diffs_fair)):
                pa, pb = qa[i], qb[i]
                if pa is None or pb is None:
                    continue
                diffs.append(abs(pa / pb - 1) * 1e4)
                if same == "buy" and pa == pb:
                    same_buy += 1
                if same == "sell" and pa == pb:
                    same_sell += 1
        t += 1.0
    print(row("price lines", len(a.quotes), len(b.quotes)))
    if both:
        print(f"  seconds with both quoting: {both}; the same buy {same_buy / both:.1%}, the same sell {same_sell / both:.1%}")
        for name, diffs in (("buy", diffs_buy), ("sell", diffs_sell), ("fair bid", diffs_fair)):
            if diffs:
                diffs.sort()
                print(f"  {name} apart: median {statistics.median(diffs):.3f} bp, p90 {diffs[int(0.9 * (len(diffs) - 1))]:.3f} bp, "
                      f"max {diffs[-1]:.3f} bp")

    print("fills matched (same side and price within 1 s)")
    matched = 0
    used: set[int] = set()
    for ta, side, _, price in a.fills:
        for j, (tb, side_b, _, price_b) in enumerate(b.fills):
            if j not in used and side == side_b and price == price_b and abs(ta - tb) <= 1.0:
                used.add(j)
                matched += 1
                break
    print(f"  {matched} of {len(a.fills)} ({na}) and {len(b.fills)} ({nb})")

    print("health (medians of the minute lines)")
    for name in HEALTH_PARTS:
        va, vb = a.health.get(name), b.health.get(name)
        if va or vb:
            print(row(name, statistics.median(va) if va else "-", statistics.median(vb) if vb else "-"))


if __name__ == "__main__":
    main()
