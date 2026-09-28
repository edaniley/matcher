
# Matching Engine Tests
Tests for the single-market matching engine in `include/hw/matcher/`.

---

## 1. Test objectives

The tests show that the engine meets the exercise requirements. Each requirement maps to specific tests.

| # | Requirement | Where it is verified |
|---|---|---|
| O1 | **Price-time priority**: best price first, earliest order first within a price level | `EngineMatchingTests`, `price_time_priority` scenario |
| O2 | **Partial fills**: a partially filled GTC order keeps its original priority; an unfilled GTC remainder rests | `EnginePartialFillTests`, `partial_fills` scenario |
| O3 | **IOC and market orders**: fill what is possible, cancel the remainder, never rest | `EngineIocMarketTests`, `ioc_and_market` scenario |
| O4 | **Self-trade prevention**: an account never trades with itself, under each policy | `EngineSelfTradeTests`, `self_trade_cancel_*` scenarios |
| O5 | **Cancel** (including cancels around a match) behaves correctly for every ordering of cancel and match | `EngineCancelTests`, `EngineCancelDuringMatchTests`, `cancel_edge_cases` scenario |
| O6 | **Queries**: top of book, full depth, and status of one order | `EngineQueryTests`, `OrderBookTests`, `snapshots` scenario |
| O7 | **Determinism**: the same inputs always give the same fills and book states | `EngineDeterminismTests`, `ReplayTests/ReplayIsDeterministic`, every scenario is run twice |
| O8 | **Input validation**: invalid orders are rejected without changing the book | `EngineValidationTests`, `validation_and_errors` scenario |
| O9 | **Decimal prices** (value + decimals) are exact, and equal values at different scales are the same price | `PriceTests`, `EngineDecimalPriceTests`, `decimal_prices` scenario |
| O10 | **Clean interface**: the engine can be driven with no custom code, JSON in and JSON out | `MatcherReplay` tool, `JsonParseTests`, `JsonFormatTests`, `ReplayTests`, all scenarios |
| O11 | **No heap on the critical path**: processing requests never allocates; all memory is sized by `EngineConfig` and allocated once when the engine is built | `NoHeapTests` |
| O12 | **Bounded capacity**: when the order pool or level pool is full, requests fail cleanly and deterministically, and pool accounting stays exact | `EngineCapacityTests`, `OrderBookTests/LevelPoolExhaustion`, `capacity_limits` scenario |

---

## 2. Test design

### 2.1 Three layers

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ 3. Scenario (golden) tests       scenarios/*.input.jsonl  → MatcherReplay │
 │    black box, no C++ needed      diff against *.expected.jsonl (CTest)    │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ 2. Harness tests                 JsonParseTests, JsonFormatTests,         │
 │    JSON codec + replay driver    ReplayTests                              │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ 1. Unit tests                    Price / Order / PriceLevel / OrderBook   │
 │    direct C++ API                Engine* suites (process() + Recorder)    │
 │                                  NoHeapTests (counting operator new)      │
 └──────────────────────────────────────────────────────────────────────────┘
```

1. **Unit tests** call the C++ API directly. Engine tests submit requests with `MatchingEngine::process()` and check the exact sequence of events, and they check book state through `top()`, `depth()` and `status()`.
2. **Harness tests** check the JSON layer on its own: parsing, error messages, the exact output format, and the replay driver's behavior (comments, error recovery, `config`, snapshots).
3. **Scenario tests** treat the engine as a black box. A scenario is a JSON Lines file of commands plus the expected output. This is the "drive it with a sequence of orders without importing the code" path from the exercise, and it doubles as a regression suite.

### 2.2 Design principles

- **Check exact event sequences.** Engine tests compare the full order of events (for example `accepted → trade → trade → rested`), not just the final state, because the order of fills is part of the contract.
- **Each test isolates one rule.** Every test case sets up the smallest book that shows the rule, e.g. a later order at a better price beating an earlier order at a worse price (`PricePriorityBeatsTimePriority`).
- **Randomized tests use fixed seeds.** `randomRequests(seed, n)` makes a repeatable mix of limit/market, GTC/IOC and cancel requests, deliberate duplicate ids, 5 accounts (so self-trades happen), and prices written at two different scales. A failure always reproduces.
- **Invariant checks back up the example-based tests.** `RandomizedInvariants` runs thousands of random requests under each self-trade policy. After every request it checks that:
  - the book is not crossed (best bid < best ask)
  - no account trades with itself, and every trade quantity is > 0
  - every trade is at the resting order's price and within the incoming order's limit
  - an accepted order ends with at most one closing event (`rested` or `canceled`), and if it has none, it is `filled`

  At the end it checks that every order's `filled` equals the sum of its trade quantities and is ≤ its size.
- **Determinism is checked two ways:**
  - at the API level, by running the engine twice on 20,000 random requests and comparing the event streams (`Event` has `operator==`)
  - at the byte level, by replaying generated JSON twice and comparing output strings, and by `RunScenario.cmake` running every scenario twice
- **Heap use is measured, not assumed.** `NoHeapTest.cpp` replaces the global `operator new` / `delete` for the test binary with a version that counts allocations while a flag is set. The engine then processes tens of thousands of random requests, including runs with tiny pools where capacity limits and retention evictions happen constantly, and the test requires **zero** allocations. `CounterDetectsAllocations` checks that the counter itself works. As a mutation check, removing the book's `reserve()` calls makes the test fail.
- **Pool accounting is checked on every request.** `RandomizedUnderCapacityPressure` checks that orders in the pool always equal resting orders plus retained orders, and never exceed `maxOrders`; that retained orders never exceed `retainedOrders`; that price levels never exceed `maxLevels`; and that the heap fallback is never used.
- **Expected files are reviewed by hand.** Every line of the expected scenario output was checked by hand against the matching rules, not just copied from the engine's output. The harness was also checked the other way: changing one value in an expected file makes the test fail with a diff.

### 2.3 Cancel during a match

The engine is a single-threaded actor, so a cancel can never land *in the middle of* a match. It is applied either entirely before or entirely after. `EngineCancelDuringMatchTests` fixes the visible result for each case:

| Case | Expected outcome |
|---|---|
| Cancel arrives after the resting order was fully filled | `cancel_rejected` / `unknown_order`; status stays `filled` |
| Cancel arrives before the match | order removed; the incoming order matches the next order in the queue |
| Cancel after a partial fill | `canceled` with the remaining quantity; earlier fills kept |
| Cancel of an IOC order that never rested | `cancel_rejected` |
| Cancel of an order already removed by self-trade prevention | `cancel_rejected` |
| Cancel of the rested remainder of a partially filled incoming order | `canceled` with that remainder |

---

## 3. Implementation

### 3.1 Files

| File | Contents |
|---|---|
| `MatcherTest.cpp` | Boost.Test module definition (`BOOST_TEST_MODULE MatcherTest`, `boost/test/included/unit_test.hpp`) |
| `PriceTest.cpp` | `PriceTests` |
| `OrderBookTest.cpp` | `OrderTests`, `PriceLevelTests`, `OrderBookTests` |
| `EngineTest.cpp` | `Engine*` suites |
| `JsonReplayTest.cpp` | `JsonParseTests`, `JsonFormatTests`, `ReplayTests`; also compiles Boost.JSON (`boost/json/src.hpp`) for the test binary |
| `NoHeapTest.cpp` | `NoHeapTests`; replaces global `operator new`/`delete` in the `MatcherTest` binary with counting versions |
| `TestHelpers.hpp` | `limit()` / `market()` request builders, `Recorder` event sink, `EngineFixture`, `randomRequests()`, stream operators so Boost.Test prints readable values on failure |
| `JsonCodec.hpp` | JSON ↔ requests, events and query results (Boost.JSON, header-only) |
| `Replay.hpp` | `Replay` driver: reads JSON Lines commands, writes JSON Lines results |
| `MatcherReplay.cpp` | Command-line wrapper around `Replay`; also compiles Boost.JSON for this binary |
| `RunScenario.cmake` | CTest runner for one scenario: replay twice → check the two runs match → compare with expected, printing a diff on mismatch |
| `CMakeLists.txt` | Targets `MatcherTest` and `MatcherReplay`; one CTest entry per scenario file |
| `scenarios/` | `<name>.input.jsonl` + `<name>.expected.jsonl` pairs |

Both executables are built with `-Wall -Wextra -fsanitize=address`.

### 3.2 Unit test suites (98 test cases)

| Suite | Cases | Covers |
|---|---:|---|
| `PriceTests` | 9 | comparison across scales, validity, `normalized()`, `rescale()` including overflow and precision loss, `toString()`, extreme values |
| `OrderTests` | 4 | `fill()` status changes, `crosses()` for buy/sell/market, `cancel()`, equality ignores list membership |
| `PriceLevelTests` | 2 | FIFO order, total quantity bookkeeping, a partial fill keeping priority, destructor unlinks orders |
| `OrderBookTests` | 9 | empty book, sort order with levels inserted at the top, middle and bottom, level totals, fills, removing levels from the middle, level pool exhaustion, priority-order iteration, `clear()`, 20k random operations against a `std::map` model |
| `EngineValidationTests` | 6 | invalid size, price, decimals and time in force; duplicate ids (resting, finished, rejected); rejects leave the book unchanged |
| `EngineMatchingTests` | 10 | resting, non-crossing orders, full fill, maker price, price priority on both sides, time priority, price beating time, multi-level sweep, event sequence numbers |
| `EnginePartialFillTests` | 4 | maker keeps priority, remainder rests, queue position of the rested remainder, repeated partial fills |
| `EngineIocMarketTests` | 7 | IOC partial / none / full, IOC limit respected, market sweep, market remainder, market on an empty book |
| `EngineSelfTradeTests` | 7 | all three policies, self-trade after trading with other accounts, same account not crossing, market order self-trade |
| `EngineCancelTests` | 5 | cancel resting, unknown id, cancel twice, partially filled, other orders keep priority |
| `EngineCancelDuringMatchTests` | 6 | see §2.3 |
| `EngineDecimalPriceTests` | 2 | equal prices at different scales share a level and FIFO order; crossing decided by numeric value |
| `EngineQueryTests` | 2 | top/depth contents, status through an order's life |
| `EngineDeterminismTests` | 2 | same input gives the same output (20k requests); randomized invariants (5k requests × 3 policies) |
| `JsonParseTests` | 5 | full and default fields, market without price, all commands, error messages |
| `JsonFormatTests` | 2 | exact serialized form of every event and query |
| `ReplayTests` | 6 | comments/blank lines, error line numbers and recovery, `config` placement, snapshots, status output, byte-level determinism |
| `EngineCapacityTests` | 7 | order pool full of resting orders, invalid order on a full pool, retention eviction and id reuse, zero retention, level pool full, destruction with live orders, randomized accounting under capacity pressure |
| `NoHeapTests` | 3 | counter self-check; zero allocations over 50k random requests; zero allocations at capacity limits (3 retention settings) |

### 3.3 Scenario tests

| Scenario | Shows |
|---|---|
| `price_time_priority` | sweeping across levels in price order, FIFO within a level, stopping at the limit, price improvement |
| `partial_fills` | a partially filled maker keeps its place; the remainder of an incoming order rests at its own limit |
| `ioc_and_market` | IOC partial/none, market sweep and remainder, market on an empty book, market + GTC rejected |
| `self_trade_cancel_newest` | incoming order canceled after trading with other accounts |
| `self_trade_cancel_oldest` | own resting order canceled; incoming order keeps matching, then rests |
| `self_trade_cancel_both` | both canceled; other accounts' orders untouched |
| `cancel_edge_cases` | every cancel ordering from §2.3 |
| `decimal_prices` | mixed scales in one level, maker's price format in trades, decimals > 18 rejected |
| `validation_and_errors` | engine rejects, and parse errors with line numbers (processing continues) |
| `snapshots` | `config` with `snapshot: top`: a top-of-book snapshot after every request |
| `capacity_limits` | tiny pools set with `config`: remainder canceled when no price level is free, retired order evicted to make room, reject when every slot holds a resting order |

---

## 4. User guide

### 4.1 Build and run

```bash
cd build
cmake ..
make MatcherTest MatcherReplay

ctest                                   # everything: unit tests + all scenarios
ctest -R MatcherScenario                # scenarios only
ctest -R MatcherScenario.partial_fills --output-on-failure   # one scenario, show diff

./test/matcher/MatcherTest                                  # all unit tests
./test/matcher/MatcherTest --run_test=EngineSelfTradeTests  # one suite
./test/matcher/MatcherTest --run_test=EngineMatchingTests/TimePriorityWithinLevel
./test/matcher/MatcherTest --log_level=test_suite           # verbose
./test/matcher/MatcherTest --list_content                   # list suites and cases
```

Boost is expected at `~/Toolbox/boost_1_90_0` (see the top-level `CMakeLists.txt`).

### 4.2 Driving the engine with `MatcherReplay`

```bash
MatcherReplay [--symbol S] [--stp cancel_newest|cancel_oldest|cancel_both]
              [--snapshot none|top|depth] [--max-orders N] [--max-levels N]
              [--retained-orders N] [input.jsonl]
```

It reads commands from the file, or from stdin if no file is given, and writes results to stdout. A summary line (`lines=… commands=… errors=…`) goes to stderr. The exit code is 0 even when some input lines have errors, because those errors are part of the output.

```bash
./test/matcher/MatcherReplay ../test/matcher/scenarios/price_time_priority.input.jsonl

echo '{"op":"new","id":1,"account":1,"side":"buy","price":6543210,"decimals":2,"size":5}
{"op":"top"}' | ./test/matcher/MatcherReplay --snapshot depth
```

### 4.3 Input format (JSON Lines)

One JSON object per line. Blank lines and lines starting with `#` are ignored. Enum values are case-insensitive. Unknown fields are errors, so typos are caught.

| Command | Fields |
|---|---|
| `new` | `id`, `account`, `side` (`buy`/`sell`), `size` — required<br>`type` (`limit`/`market`, default `limit`), `tif` (`gtc`/`ioc`, default `gtc`)<br>`price` (integer, required for limit orders), `decimals` (0–18, default 0) |
| `cancel` | `id` |
| `top` | — |
| `depth` | `levels` (optional; default all) |
| `status` | `id` |
| `config` | `symbol`, `stp`, `snapshot`, `max_orders`, `max_levels`, `retained_orders` — must come before any other command |

A price is the integer `price` together with `decimals`: `"price":6543210,"decimals":2` is 65432.10. Market orders must be `ioc`.

### 4.4 Output format

Every line is one JSON object, with keys always in the same order:

| Record | Example |
|---|---|
| order accepted | `{"seq":1,"event":"accepted","id":1}` |
| order rejected | `{"seq":2,"event":"rejected","id":2,"reason":"invalid_price"}` |
| trade | `{"seq":3,"event":"trade","maker":1,"taker":2,"maker_account":1,"taker_account":2,"taker_side":"buy","price":100,"decimals":0,"qty":3,"maker_remaining":2,"taker_remaining":0}` |
| remainder rested | `{"seq":4,"event":"rested","id":2,"price":100,"decimals":0,"remaining":7}` |
| order canceled | `{"seq":5,"event":"canceled","id":2,"remaining":7,"reason":"user_requested"}` (`user_requested` / `ioc_remainder` / `self_trade` / `capacity_exceeded`) |
| cancel rejected | `{"seq":6,"event":"cancel_rejected","id":9,"reason":"unknown_order"}` |
| query result | `{"query":"top","bid":{"price":100,"decimals":0,"qty":5,"orders":2},"ask":null}` |
| automatic snapshot | same as a query result, with key `"snapshot"` instead of `"query"` |
| input error | `{"error":"unknown field 'colour'","line":12}` |

`seq` is the engine's event sequence number. It starts at 1 and increases by one with no gaps. For each new order, events are emitted in this order: `accepted`, then `trade` / self-trade `canceled` events as matching happens, then at most one `rested` or `canceled` for the remainder.

### 4.5 Adding a scenario

1. Create `scenarios/<name>.input.jsonl`. Use `#` comments to state what the scenario shows and what you expect.
2. Generate a candidate expected output:
   ```bash
   ./test/matcher/MatcherReplay ../test/matcher/scenarios/<name>.input.jsonl \
       > ../test/matcher/scenarios/<name>.expected.jsonl
   ```
3. **Check every line of the expected file by hand** against the matching rules. Only then commit it. Otherwise the scenario just records whatever the engine did.
4. Re-run `cmake ..` (the scenario list is globbed with `CONFIGURE_DEPENDS`, so a plain `make` usually picks it up) and run `ctest -R MatcherScenario.<name>`.

When a change to the engine intentionally alters behavior, regenerate the affected expected files the same way and review the diff (`git diff scenarios/`) before committing.

### 4.6 Adding a unit test

Add a case to the relevant suite in `EngineTest.cpp` (or a new `*Test.cpp` listed in `CMakeLists.txt`). The usual pattern:

```cpp
BOOST_FIXTURE_TEST_CASE(MyCase, EngineFixture) {        // EngineFixture: engine + Recorder
  submit(limit(1, /*account*/1, Side::Sell, /*price*/100, /*size*/5));
  auto & r = submit(limit(2, 2, Side::Buy, 100, 3));   // r holds only this request's events
  BOOST_REQUIRE_EQUAL(r.events.size(), 2u);
  BOOST_CHECK(r.is<OrderAccepted>(0));
  BOOST_CHECK_EQUAL(r.at<Trade>(1).qty, 3);
  BOOST_CHECK_EQUAL(engine.status(1)->remaining(), 2);
}
```

For a non-default self-trade policy, construct `EngineFixture f(SelfTradePolicy::CancelOldest);` inside a `BOOST_AUTO_TEST_CASE`.

---

## 5. Known limitations and notes

- **Depth level price format:** a depth level shows its price in the decimals of the order that created the level. The value is always correct, but after that order leaves, the format may differ from the orders still resting there.
- **Order ids and retention:** an id is a duplicate while the engine still holds that order, resting or finished. Finished orders (filled, canceled or rejected) are kept only while they fit the retention window (`retained_orders`, default unlimited) and the order pool (`max_orders`, default 65,536). The oldest is evicted first. After eviction, `status` returns `null` and the id can be reused.
- **Capacity:** if every order slot holds a resting order, a new order is rejected with `capacity_exceeded`. If a remainder needs a new price level and all `max_levels` (default 4,096) are in use, the remainder is canceled with `capacity_exceeded`.
- **Heap use:** engine construction, the JSON harness and the tests' `Recorder` allocate. `MatchingEngine::process()` does not (see `NoHeapTests`). Its own event sink must also avoid allocating if the whole path is to stay heap-free.
- **Changes during the switch to heap-free containers:** all engine, JSON and scenario tests passed unchanged when `std::map`, `std::list` and `std::unordered_map` were replaced with the `std::vector<PriceLevel*>` levels, the intrusive `DList` and the `AllocatorTrivial` / `HashTableTrivial` pools. Only the `PriceLevel` and `OrderBook` unit tests were rewritten, because their API changed (the book now links caller-owned orders instead of copying them).
- **What is not tested:** concurrency (by design, the engine is single-threaded), performance, and the assert-only checks inside the containers (Boost.Test has no death tests).
