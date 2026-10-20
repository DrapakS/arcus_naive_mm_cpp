# arcus_naive_mm_cpp

[arcus_naive_mm](https://github.com/DrapakS/arcus_naive_mm), переписанный на C++20: тот же наивный
маркетмейкер для перпов [Arcus](https://docs.arcus.xyz) — котирует рынок Arcus по стакану и сделкам
Binance (спот, фьючерс), Bybit, OKX и самого Arcus, режимы `off`/`quote`/`paper`/`live`.

Порт сделан с Python-версии на коммите `58cacaf` (отстающие соединения, `[open_halt]`,
`[portfolio]`, `[vol_markup]`, 19 октября) и повторяет её поведение один в один: те же
конфиги (они лежат здесь же, `config*.toml`), те же ключи командной строки, те же формулы, тики и
округления, те же защиты, те же строки в логах — вплоть до текста. Поэтому всё, что README
Python-версии рассказывает о прайсинге, защитах, торговле, ML, ключах и логах, верно и здесь;
ниже — только то, что касается C++.

## Сборка

Нужны: GCC 13+ (C++20, `<format>`, часовые пояса `std::chrono`), CMake 3.20+, Boost 1.81+
(Asio, Beast, JSON), OpenSSL 1.1.1+ (TLS и подпись Ed25519). Тесты — GoogleTest (без него
собирается только бот). [toml++](https://github.com/marzer/tomlplusplus) лежит в `third_party/`.

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build libboost-all-dev libssl-dev libgtest-dev   # Ubuntu 24.04+ (24.04: и g++-13)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nmm_tests            # 421 тест, ~50 с
```

- Компилятор CMake берёт сам: самый новый GCC 13+ из `g++`, `c++`, `g++-16`…`g++-13` (другой — через
  `CXX=...` или `-DCMAKE_CXX_COMPILER=...`); GCC старше 13 не годится (нет `<format>`). Самый новый —
  потому что системный Boost собран системным GCC: Boost 1.90, собранный GCC 15, с `g++-13` не
  линкуется (`undefined reference to __cxa_call_terminate@CXXABI_1.3.15`).
- Если `cmake` пишет, что Boost не линкуется с выбранным GCC, — поставьте штатный `g++` (`sudo apt install g++`).
- После неудачной настройки — `rm -rf build`: CMake запоминает в `build/CMakeCache.txt` и
  компилятор, и его отсутствие.
- Память: одна компиляция берёт до ~2 ГБ (`net.cpp`; `agent.cpp` ~700 МБ). С Ninja CMake сам
  ограничивает число одновременных компиляций — по одной на 2 ГБ, свободных при настройке
  (`Compiles at once: N` в её выводе; задать руками — `-DNMM_COMPILE_JOBS=1`), а при свободных
  < 4 ГБ GCC чаще собирает мусор (пик ~1,2 ГБ). Иначе компилятор убивает OOM killer:
  `Killed signal terminated program cc1plus`. Свободно меньше ~1,5 ГБ — добавьте swap и соберите
  только бота, без тестов:
  ```bash
  sudo fallocate -l 4G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile
  cmake --build build --target naive_mm
  ```
- Ubuntu 22.04 не подойдёт как есть: там GCC 11 и Boost 1.74.

Версия бота (коммит, ветка, есть ли незакоммиченные правки, отпечаток исходников) вшивается при
каждой сборке и пишется в лог при старте и раз в час — как у Python-версии.

## Запуск

Как у Python-версии, только вместо `python -m naive_mm` — `build/naive_mm`:

```bash
cp config.example.toml config.toml
build/naive_mm --config config.toml --ladder                 # обе лестницы на каждый апдейт
build/naive_mm --config config.toml --trade quote            # цены покупки/продажи без ордеров
build/naive_mm --config config.toml --trade paper --orders-log paper.log
build/naive_mm --config config.toml --trade live --credentials ~/.arcus/credentials.toml
build/naive_mm --config config.btc-both.toml --set drift.markup_factor=1 --trade paper
build/naive_mm --config config.toml --check                  # только проверить конфиг: 0 — всё хорошо
```

- Ключи: `--config`, `--ladder`, `--log-level`, `--trade {off,quote,paper,live}`, `--credentials`,
  `--orders-log`, `--color {auto,always,never}`, `--set SECTION.KEY=VALUE` (сколько угодно раз).
  Новый только `--check`: загрузить конфиг с `--set`, проверить и выйти, ничего не подключая.
- Останавливать так же: Ctrl+C, SIGTERM или SIGHUP — бот снимает ордера, ждёт ответов до 3 с,
  выключает dead man's switch и пишет `=== AT EXIT`. Не `kill -9`.
- Прокси — как у aiohttp с `trust_env`: `https://` идёт через `HTTPS_PROXY`, `wss://` — только
  через `WSS_PROXY` (не `HTTPS_PROXY`), `ws://`/`http://` — через `WS_PROXY`/`HTTP_PROXY`;
  `NO_PROXY` соблюдается.
- Отстающие соединения переподключаются, как в Python (`naive_mm/race.py` → `src/race.hpp`):
  каждая копия сверяется с первой копией того же обновления; соединение, отстающее по медиане за 2 мин
  больше `[agent] slow_link_ms` (30 мс; 0 — выключено), пока другое на связи, переподключается
  (`!!! link 2/2 slow: ...: reconnecting`; в статусе `links 2/2 first 96/3% behind 0/8ms`). Бумажный
  шлюз берёт сделки Arcus по `arcus.connections` соединениям, live — ещё и по cancel-lanes.
- OKX (`[okx]`, в ETH-конфигах включён) по умолчанию идёт на порт 8443; если он закрыт,
  `--set okx.ws_url='"wss://ws.okx.com"'` — тот же поток на 443.
- Binance с HTTP 451 — те же зеркала: `--set binance.ws_url='"wss://data-stream.binance.vision"'
  --set binance.rest_url='"https://data-api.binance.vision"'` (или в конфиге).
- ML-модели: LightGBM читается без библиотеки LightGBM — из текстового формата или из pickle
  (строка модели достаётся из опкодов pickle, сам pickle не исполняется); линейные — как в Python.
  Деревья обходятся по правилам LightGBM, предсказание совпадает до последнего бита
  (`tests/data/lgbm` — эталоны, посчитанные самим LightGBM).
- `scripts/paper_ab.sh` — тот же A/B на бумаге, только запускает C++-бинарник (`BIN`, по умолчанию
  `build/naive_mm`).

### Live рядом с Python-ботами

`config.btc-ml-both.toml` в этом checkout — живой BTC ML набор владельца, `config.btc-ml-okx-only.toml` как
есть (19 октября вечером; Python-овский `config.btc-ml-both.toml` — модель с гардами `config.btc-both.toml`,
им шла пара `ml-both`). Его проверяет `tests/test_live_19oct.cpp`.

```bash
cd ~/arcus_naive_mm_cpp && git pull && cmake --build build
build/naive_mm --check --config config.btc-ml-both.toml
nohup build/naive_mm --config config.btc-ml-both.toml --trade live \
    --orders-log live-btc-ml-cpp.log > /dev/null 2> live-btc-ml-cpp.err &
sudo bash scripts/pin_bots.sh          # копия из этого checkout: знает config.btc-ml-both.toml (ядро 2)
```

- Один BTC-бот на субаккаунт: перед запуском остановить Python-бота на BTC-USD (`btc-both` или
  `btc-ml-okx-only`); позицию бот берёт с биржи, два бота на одном рынке мешают друг другу.
- `[portfolio]`: файлы позиций в `~/.naive_mm/portfolio/live/<рынок>.<pid>.json`, формат Python-ботов
  байт в байт; C++ и Python читают друг друга. Запускать от того же пользователя, что и Python-боты
  (тот же `~`, не через `sudo`). В строке статуса видно, кого бот видит:
  `portfolio crypto: ETH-USD +1,248 $ (x1.3, 0 s ago), ...`; при запуске — `portfolio crypto: BTC-USD weighs x1 ...`.
- Ключи — тот же файл, что у Python (`~/.config/naive_mm/credentials.toml`, `$NAIVE_MM_CREDENTIALS` или
  `--credentials`). Остановка — `kill <pid>` (SIGTERM): ордера снимаются, `=== AT EXIT` в логе.

## Устройство

Один поток с `boost::asio::io_context` вместо цикла asyncio: каждая корутина Python стала
`asio::awaitable`, `asyncio.Event`/`create_task`/`call_later`/`sleep` — их аналогами из
`src/util/aio.hpp`. Пачка кадров, уже прочитанных из сокетов, обрабатывается до того, как
стратегия пересчитает котировки — как в asyncio, где лента дочитывает всё, что лежит в сокете,
прежде чем очередь дойдёт до цикла стратегии. Логи пишут отдельные потоки (как в Python), так
что остановленный терминал или медленный диск цикл не держат.

Цены и размеры — `Decimal` (`src/util/decimal.hpp`): фиксированная точка, 18 знаков после
запятой, 128 бит, округление half-even — там же, где Python берёт `decimal.Decimal`; там, где
Python считает во `float`, C++ считает в `double`, а печатает так же, как Python (`repr`, `{:g}`).

| Python (`naive_mm/`)        | C++ (`src/`)                                             |
|-----------------------------|----------------------------------------------------------|
| `__main__.py`               | `main.cpp`, `logsetup.*`                                 |
| `config.py`                 | `config.*`                                               |
| `agent.py`                  | `agent.*`                                                |
| `orderbook.py`, `view.py`   | `orderbook.*`, `view.hpp`                                |
| `pricing.py`                | `pricing.*`                                              |
| `ml.py`, `lgbm.py`          | `ml.*`, `lgbm.*`                                         |
| `health.py`                 | `health.*`                                               |
| `feeds/*.py`                | `feeds/*` (базовая лента, Binance спот/фьючерс/сделки, Bybit, OKX, Arcus) |
| `trading/*.py`              | `trading/*` (шлюз Arcus и бумажный, стратегия, защиты, сессии, round trips, подпись) |
| aiohttp, orjson, PyNaCl     | `net/net.*` (Beast + OpenSSL), `util/json.*` (Boost.JSON), `trading/signing.*` (OpenSSL Ed25519) |
| asyncio, logging            | `util/aio.*`, `util/log.*`, `util/pyfmt.*` (форматирование как в Python) |

Тесты (`tests/`) перенесены с pytest: фейковые биржи (`fake_server.*`, `fake_arcus_trading.*`)
поднимают локальные HTTP/WebSocket-серверы, как фикстуры Python-версии.

## Сравнение с Python-версией

### Бумага бок о бок

`bench/paper_pair.sh` запускает Python- и C++-бота на бумаге одновременно, с одним конфигом,
на одном живом рынке, каждого со своим `--orders-log`; `bench/compare_logs.py` сводит два лога
за общее время:

```bash
bash bench/paper_pair.sh start basis        # BTC, basis-прайсинг, long: config.recommended.toml
bash bench/paper_pair.sh start basis-both   # BTC, basis-прайсинг, обе стороны: config.btc-both.toml
bash bench/paper_pair.sh status
bash bench/paper_pair.sh compare            # в любой момент; = python3 bench/compare_logs.py A.log B.log
bash bench/paper_pair.sh stop               # SIGTERM обоим, как Ctrl+C
```

Именованные пары — как боты в `scripts/paper_ab.sh`: `basis`, `basis-both`, `ml`, `ml-both` (здесь — живой
набор, см. «Live рядом с Python-ботами»),
`ml-okx-only` (`config.btc-ml-okx-only.toml`: BTC ML, из гардов только OKX), `eth-basis-both`, `eth-ml-both`, `gld`,
`spy-both` (`config.spy-both.toml`: SPY с двух сторон, базовый конфиг для бумаги, есть только в этом checkout); можно и любой конфиг: `start config.sol-both.toml sol`. Оба
бота читают один и тот же файл (из этого checkout), Python-версия должна быть на коде, с которого
сделан порт (ветка `claude/funny-wright-a95ao3`, `58cacaf` или новее), — `start` печатает коммиты
обоих, а `compare` — версии из логов. За гео-блоком Binance — те же зеркала через `SETS`:
`SETS='binance.ws_url="wss://data-stream.binance.vision" binance.rest_url="https://data-api.binance.vision"'`.
Каждому боту — своё ядро: `start` предупреждает, когда ботов на хосте больше, чем ядер.

`PY_REPO` — checkout Python-версии (по умолчанию `../arcus_naive_mm`), `PY` — его интерпретатор
(по умолчанию `$PY_REPO/.venv/bin/python`), `BIN` — C++-бинарник. Логи — `paper-pair/<имя>-py.log`
и `-cpp.log`, логи прошлого запуска переименовываются с временем. Сравнение печатает рядом:
выставления и отмены по причинам, исполнения и финальные состояния ордеров, round trips и их
результат, срабатывания защит (`DROP`/`RISE`/`TAKE`/`LATE`/`DRIFT`/`FALL`/`RALLY`), насколько
совпадают цены покупки/продажи и fair bid посекундно, совпавшие исполнения и медианы строк
здоровья (лаг цикла, CPU, задержки чтения лент). `compare_logs.py` можно натравить на любые два
orders-лога (`--names`, `--from`, `--to`).

Пара на BTC-USD (`config.btc-both.toml`, бумага, живой рынок, 19 октября 2026, 12,5 мин):

|                                        | Python        | C++           |
|----------------------------------------|---------------|---------------|
| выставлено покупок / продаж            | 186 / 218     | 185 / 226     |
| отмен (из них «target is N bp away»)   | 397 (238)     | 402 (239)     |
| исполнений (совпавших с другим ботом)  | 17 (16)       | 17 (17)       |
| round trips, результат, средний bp     | 10, $0.0150, 1.99 | 10, $0.0141, 1.99 |
| AT EXIT                                | short 0.00011843, +0.86 bp | то же |
| DROP / RISE / TAKE / DRIFT / LATE      | 204 / 118 / 176 / 43 / 6 | 204 / 116 / 176 / 43 / 8 |
| секунд с той же ценой покупки / продажи | 89.0% / 89.4% (медиана расхождения 0 bp, p90 0.012 bp) | |
| CPU (медиана строк здоровья)           | 23.5%         | 9%            |
| лаг цикла p99 / max, мс                | 2 / 54        | 0.6 / 40      |
| чтение фьючерса p50 / p99, мс          | 70 / 99       | 70 / 79       |

Расхождения — в мелочах времени: C++ разбирает кадр быстрее и видит промежуточные состояния
стакана, которые Python, ещё занятый предыдущим кадром, склеивает с последующими, — поэтому строк
цен у него больше (2088 против 1701), а отдельные защиты и исполнения приходятся на кадр раньше
или позже.

### Скорость

`bench/compare.sh` гоняет ботов по одному против `bench/replay_server.py` — локальной «биржи»,
которая шлёт поток оживлённого рынка BTC (стакан и bookTicker спота, сделки, bookTicker
фьючерса, стакан и сделки Arcus, REST-снимки) с множителем `RATE` (×1 ≈ 730 кадров/с на все
ленты; с двумя соединениями на ленту, как в `config.btc-both.toml`, бот получает ≈1400 кадров/с).
После разгона меряет процессорное время бота (все его потоки) на кадр, долю ядра, память и
задержки из строки здоровья:

```bash
PY=../arcus_naive_mm/.venv/bin/python bash bench/compare.sh                 # оба бота, ×1 и ×4
RATES="1 4 16" DURATION=60 bash bench/compare.sh
BOTS=cpp CONFIG=config.btc-ml-both.toml bash bench/compare.sh
```

Замер на 4-ядерной облачной машине (19 октября 2026, без другой нагрузки; Python 3.13 с uvloop и
orjson, C++ — Release, GCC 13), по 60 с после 20 с разгона:

|                         | Python ×1 | C++ ×1  | Python ×4 | C++ ×4  |
|-------------------------|-----------|---------|-----------|---------|
| кадров/с                | 1399      | 1399    | 5537      | 5531    |
| CPU на кадр             | 135 мкс   | 47 мкс  | 89 мкс    | 25 мкс  |
| доля ядра               | 18.9%     | 6.5%    | 49.5%     | 14.0%   |
| память (RSS)            | 52 МБ     | 14 МБ   | 52 МБ     | 14 МБ   |
| лаг цикла p99           | 1.0 мс    | 0.5 мс  | 2.0 мс    | 0.5 мс  |
| чтение фьючерса p99 / max | 1.9 / 7.2 мс | 1.3 / 4.4 мс | 2.5 / 6.7 мс | 1.6 / 4.4 мс |

То есть C++ тратит в 3–3.5 раза меньше процессора на кадр и в 3.7 раза меньше памяти, а его цикл
реже и меньше задерживает чтение лент. На живом рынке (пара выше) — 9% ядра против 23.5%.
Около трети времени C++ — системные вызовы (epoll, recv на каждый кадр), из остального больше
всего берёт разбор JSON (~¼), затем пересчёт котировок и разбор кадров лентами.
