# hw::matcher Design Notes

## The core data structures for the book

- **Price levels:** each side is a `std::vector<PriceLevel*>` sorted from worst to best price, so the best level is the last element.
  - Why: almost all activity happens at or near the best price. Reading or removing the top level is O(1), and adding a level near the top shifts only a few pointers. Other lookups are a binary search over contiguous, cache-friendly memory.
  - The vectors reserve their full capacity at startup, so they never reallocate.
- **Orders within a level:** an intrusive doubly linked list (`DList`) in arrival order (FIFO).
  - Why: FIFO order is time priority. Adding an order at the back, taking one from the front and removing one from the middle (a cancel) are all O(1) and allocate nothing. A partial fill leaves the order where it is, so it keeps its priority.
- **Order lookup by id:** a fixed-capacity hash table (`HashTableTrivial`) from order id to order.
  - Why: O(1) cancel and status lookup. Its nodes come from a preallocated pool, and erase is clean, so performance doesn't degrade as orders come and go.
- **Memory:** orders and price levels come from preallocated pools (`AllocatorTrivial`).
  - Why: nothing is allocated from the heap while processing orders, which gives predictable latency. When a pool is full, the order is rejected cleanly instead of allocating.
- **Finished orders** (filled, canceled or rejected) are kept in a bounded list, oldest first, so their status can still be queried. The oldest is evicted when space is needed.
- **Prices** are fixed-point integers: a value plus a number of decimals.
  - Why: exact arithmetic with no floating-point rounding, and the same price written at different scales lands on the same level.

## How to keep the engine deterministic

- **Single-threaded actor:** there's one entry point, `process(request)`. Requests are applied one at a time in arrival order, with no shared state, locks or threads inside the engine.
- **No outside inputs:** the engine reads no clock, generates no random numbers and does no I/O. Time priority comes from a sequence number the engine assigns itself, not a timestamp.
- **Integer-only arithmetic:** prices and quantities are integers, so results are identical on every platform.
- **Ordered outputs only:** output is produced by the matching loop and the sorted price levels. The hash table is used only for lookups and never iterated, so its internal order can't leak into results.
- **Sequence-numbered events:** every event carries a sequence number that increases by one with no gaps, so the output stream is fully ordered and can be replayed.
- **Tests enforce it:**
  - 20,000 random requests run twice must produce identical event streams.
  - A generated JSON input stream replayed twice must produce byte-identical output.
  - Every golden scenario runs twice and must match itself before it's compared with its expected output.
