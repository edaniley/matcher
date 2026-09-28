#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#if defined(__SSE2__)
	#include <immintrin.h>
#endif
#ifdef DEBUG
	#include <iostream>
	#include <bitset>
#endif

namespace hw::utility::swisstable {

namespace detail {
  // copied from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
  static inline uint64_t hash(uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
  }
}

enum Control: int8_t {
  Empty   = static_cast<int8_t>(0xFF),  // -1
  Deleted = static_cast<int8_t>(0x80),  // -128
  Busy    = static_cast<int8_t>(0xFE)   // -2
};

static constexpr size_t SIMD_SIZE = 16;
enum class ThreadSafetyPolicy { Single, Multi };
enum class DuplicatePolicy    { Reject, Overwrite };



//
// https://abseil.io/about/design/swisstables
// SwissTableHashmap: fixed-capacity hash map (integer key -> pointer payload)
// - No allocations after construction
// - Open addressing with linear probing
// - SIMD-accelerated group probing of 16 control bytes (SSE2), with scalar fallback
// - Control bytes: OxFF = Empty, 0x80 = Deleted, 0x00..0x7F = 7-bit tag
//
// https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#techs=SSE_ALL


template <typename Value, size_t MAX_KEYS, DuplicatePolicy Policy>
class HashmapST {
  static_assert (!(MAX_KEYS & (MAX_KEYS - 1)));
  static_assert (MAX_KEYS >= 16);

#if defined(__SSE2__) && defined(DEBUG)
  static void print(__m128i value) {
    std::bitset<sizeof(value)*8> bs;
    std::memcpy ((void *)&bs, &value, sizeof(value));
    std::cout << bs.to_string() << std::endl;
  }
#endif

public:
  HashmapST() :_size(0) {
    _ctrl.fill(Control::Empty);
    mirror_tail_();
    _keys.fill(0);
    _values.fill(nullptr);
  }

  // returns payload pointer or nullptr
  inline Value * find (uint64_t key) const noexcept {
    const uint64_t h = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(h & 0x7F); // tag is first 7 bits
    const size_t idx = (h >> 7) & (MAX_KEYS - 1);        // starting slot

    const __m128i target = _mm_set1_epi8(tag); // 16 bytes , each set to tag
    // select and probe group starting with idx
    for (size_t i = 0; i < MAX_KEYS; i += SIMD_SIZE) {
      const size_t j = (idx + i) & (MAX_KEYS - 1);
      const __m128i group = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&_ctrl[j]));

      // _mm_cmpeq_epi8 intrinsic performs an element-wise comparison for equality on two 128-bit SIMD registers,
      //    treating them as vectors of 16 signed or unsigned 8-bit integers (bytes).
      // Input: It takes two arguments, both of type _ml28i, which are 128-bit integer vectors stored in XMM registers.
      // Oeration: It compares each of the 16 bytes in the first input vector with the corresponding byte in the secon
      //    input vector simultaneously (in parallel).
      // Output: It returns a new __m128i vector of 16 bytes. For each element:
      //    If the corresponding bytes in the input vectors are equal, the resulting byte is set to OxFF (all bits are 1).
      //    If the corresponding bytes are not equal, the resulting byte is set to 0x00 (all bits are 0).
      const __m128i matchedBytes =_mm_cmpeq_epi8(group, target);
      //_mm_movemask_epi8 intrinsic is an SSE2 instruction that takes a 128-bit integer vector
      //    containing 16 separate 8-bit elements and produces a 16-bit integer (an int) where each bit is
      //    the most significant bit (MSB) of the corresponding byte in the input vector.
      // The function extracts the most significant bit (the sign bit) from each of the 16 bytes in
      //    the input __m128i register and packs these bits into a single 16-bit integer result.
      // Input: A 128-bit SIMD register (e. g•,ml28) that holds 16 individual 8-bit values (bytes).
      // Process: For each of the 16 bytes, the intrinsic checks the 8th bit (the most significant bit).
      // Output: An int (typically 32-bit on x86-64 platforms, but only the lower 16 bits are used) where:
      //    Bit 0 of the result is the MSB of the first (least significant) byte of the input vector.
      //    Bit 1 of the result is the MSB of the second byte.
      //    ...and so on, up to bit 15, which is the MSB of the sixteenth (most significant) byte.
      const auto matchedBits = _mm_movemask_epi8(matchedBytes);
      // print (matchedBytes);
      // print (matchedBits);
      uint32_t matchMask = static_cast<uint32_t>(matchedBits);
      while (matchMask) {
        const int bit = __builtin_ctz(matchMask); // returns 1st set bit i.e. index of potential match
        const size_t entry_idx = (j + static_cast<size_t>(bit)) & (MAX_KEYS - 1);
        if (_keys[entry_idx] == key) [[likely]] {
          return _values [entry_idx];
        }
        matchMask &= (matchMask - 1); // clears lowest set bit; try next if present
      }

      // early exit if nay byte in the group is Empty
      const uint32_t emptyMask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(group, _mm_set1_epi8 (Control:: Empty))));
      if (emptyMask) [[likely]] return nullptr;
    }
    return nullptr;
  }

  // insert or update; returns false if table is fuLl
  inline bool insert(uint64_t key, Value* value) noexcept {
    const uint64_t hashval = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(hashval & 0x7F);
    const size_t idx = (hashval >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; ++ i) {
      const size_t pos = (idx + i) & (MAX_KEYS - 1);
      const int8_t ctrl = _ctrl[pos];

      // Empty or Deleted: claim slot
      if (ctrl < 0) {
        set_ctrl_(pos, tag);
        _keys   [pos] = key;
        _values [pos] = value;
        ++_size;
        return true;
      }

      if (_keys[pos] == key) {
        if constexpr (Policy == DuplicatePolicy::Reject) {
          return false;
        }
        else {
          _values[pos] = value;
          return true;
        }
      }
    }
    return false;
  }

  // erase key if exists
  inline void erase(uint64_t key) noexcept {
    const uint64_t h = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(h & 0x7F);
    const size_t idx = (h >> 7) & (MAX_KEYS - 1);
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      const size_t pos = (idx + i) & (MAX_KEYS - 1);
      const int8_t c = _ctrl[pos];

      if (c == Control::Empty) return;
      if (c == tag && _keys[pos] == key) {
        set_ctrl_(pos, Control::Deleted);
        _values[pos] = nullptr;
        if (_size) --_size;
        return;
      }
    }
  }

  inline size_t size() const noexcept { return _size; }
  static constexpr size_t capacity() noexcept { return MAX_KEYS; }

  inline void clear() noexcept {
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      _ctrl   [i] = Control::Empty;
      _keys   [i] = 0;
      _values [i] = nullptr;
    }
    mirror_tail_();
    _size = 0;
  }


  // Debug traversal
  // call f(pos, key, distance (unsuccessful atempts to insert a key) for each live entry.
  // to support writing test cases template <typename Callback>
  template <typename Callback>
  inline void for_each(Callback && callback) const noexcept {
    for (size_t pos = 0; pos < MAX_KEYS; ++pos) {
      const int8_t c = _ctrl[pos];
      if (c >= 0) {// Skip Empty/Deleted
        const uint64_t key = _keys[pos];
        if (_values[pos] == nullptr) continue;
        const uint64_t h = detail::hash(key);
        const size_t idx = (h >> 7) & (MAX_KEYS -1);
        const size_t distance = (pos + MAX_KEYS - idx) & (MAX_KEYS -1);
        callback (pos, key, distance);
      }
    }
  }

private:
  // control bytes with tail padding to allow safe 16-byte Loads near the end
  alignas (16) std::array<int8_t, MAX_KEYS + SIMD_SIZE> _ctrl;
  std::array<uint64_t, MAX_KEYS> _keys;
  std::array<Value*, MAX_KEYS> _values;
  size_t _size;

  // copy first SIMD_SIZE control bytes to tail (indices [MAX_KEYS ... MAX_KEYS+SIMD_SIZE-1])
  inline void mirror_tail_() noexcept {
    std::memcpy(&_ctrl[MAX_KEYS], &_ctrl[0], SIMD_SIZE);
  }

  // set control byte and maintain tail mirror when touching head
  inline void set_ctrl_(size_t pos, int8_t v) noexcept {
    _ctrl[pos] = v;
    if (pos < SIMD_SIZE) _ctrl[MAX_KEYS + pos] = v;
  }
};

//
// Thread-safe SwissTable (atomic ctrl/keys/values, Busy reservation)
//
template <typename Value, size_t MAX_KEYS, DuplicatePolicy Policy>
class HashmapMT {
  static_assert(!(MAX_KEYS & (MAX_KEYS - 1)), "MAX_KEYS must be a power of two");
  static_assert(MAX_KEYS >= 16, "MAX_KEYS must be >= 16");
  static_assert(std::atomic<int8_t>::is_always_lock_free);
  static_assert(std::atomic<Value *>::is_always_lock_free);
  static_assert(std::atomic<size_t>::is_always_lock_free);

public:
  HashmapMT () noexcept : _size(0) {
    for (size_t i = 0; i < MAX_KEYS + SIMD_SIZE; ++i)
      _ctrl[i].store(Control::Empty, std::memory_order_relaxed);

    mirror_tail_relaxed_();
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      _keys[i].store(0, std::memory_order_relaxed);
      _values[i].store(nullptr, std::memory_order_relaxed);
    }
  }

  inline Value* find(uint64_t key) const noexcept {
    const uint64_t h = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(h & 0x7F);
    const size_t idx = (h >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; i += SIMD_SIZE) {
      const size_t j = (idx + i) & (MAX_KEYS - 1);
      // Build masks using RELAXED loads onLy (fast path).
      uint16_t matchMask = 0, emptyMask = 0;
      for (int k = 0; k < static_cast<int>(SIMD_SIZE); ++k) {
        const int8_t c = _ctrl[j + k].load(std::memory_order_relaxed);
        matchMask |= static_cast<uint16_t>(c == tag) << k;
        emptyMask |= static_cast<uint16_t>(c == Control::Empty) << k;
      }

      // Process candidate matches first (re-verify with ACQUIRE before reading key/value).
      while (matchMask) {
        const int bit = __builtin_ctz(matchMask);
        const size_t entry_idx = (j + static_cast<size_t>(bit)) & (MAX_KEYS - 1);

        // Re-check ctrl with ACQUIRE to synchronize with publisher of this slot.
        const int8_t c2 = _ctrl[entry_idx].load(std::memory_order_acquire);
        if (c2 == tag) {
          // key was written RELAXED, safe to read after ctrl acquire
          if (_keys[entry_idx].load(std::memory_order_relaxed) == key) {
            // value was published with RELEASE; pair with ACQUIRE here
            return _values[entry_idx].load(std::memory_order_acquire);
          }
        }
        matchMask &= (matchMask - 1);
      }

      // EarLy-exit only after handling alL matches:
      // If any empty was observed, re-confirm the first empty with ACQUIRE before returning.
      if (emptyMask) [[likely]] {
        const int firstEmptyBit = __builtin_ctz(emptyMask);
        const size_t empty_idx = (j + static_cast<size_t>(firstEmptyBit)) & (MAX_KEYS - 1);

        // Re-check with acquire to avoid false negatives due to a relaxed, stale Empty.
        if (_ctrl[empty_idx].load(std::memory_order_acquire) == Control::Empty) {
          return nullptr;
        }
        // If it's no longer Empty, continue probing next groups.
      }
    }
    return nullptr;
  }

  inline bool insert (uint64_t key, Value* value) noexcept {
    const uint64_t h = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(h & 0x7F);
    const size_t idx = (h >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; ++i) {
      const size_t pos = (idx + i) & (MAX_KEYS - 1);
      int8_t ctrl = _ctrl[pos].load(std::memory_order_relaxed);

      if (ctrl == tag) {
        if (_keys[pos].load(std::memory_order_relaxed) == key) {
          if constexpr (Policy == DuplicatePolicy::Reject) {
            return false;
          }
          else {
            _values[pos].store(value, std::memory_order_release);
            return true;
          }
        }
      }

      if (ctrl == Control::Empty || ctrl == Control::Deleted) {
        if (_ctrl[pos].compare_exchange_strong(ctrl, Control::Busy,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          _keys[pos].store(key, std::memory_order_relaxed);
          _values[pos].store(value, std::memory_order_release);
          set_ctrl_(pos, tag);
          _size.fetch_add(1, std::memory_order_relaxed);
          return true;
        }
        // CAS failed, likely became Busy or another tag; retry this slot
        i--;
        continue;
      }

      if (ctrl == Control::Busy) {
          // Slot is being modified by another thread. We cannot skip it because
          // it might eventually contain our key (violating uniqueness) or be
          // the slot we need to claim.
#if defined(__SSE2__)
          _mm_pause();
#endif
          i--; // Retry this slot
          continue;
      }
    }
    return false;
  }

  inline void erase(uint64_t key) noexcept {
    const uint64_t h = detail::hash(key);
    const int8_t tag = static_cast<int8_t>(h & 0x7F);
    const size_t idx = (h >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; ++i) {
      const size_t pos = (idx + i) & (MAX_KEYS - 1);
      const int8_t c = _ctrl[pos].load(std::memory_order_acquire);
      if (c == Control::Empty) return;
      if (c == tag &&_keys[pos].load(std::memory_order_acquire) == key) {
        Value* old = _values[pos].exchange(nullptr, std::memory_order_acq_rel);
        set_ctrl_(pos, Control::Deleted);
        if (old) _size.fetch_sub(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  inline size_t size() const noexcept { return _size.load(std::memory_order_relaxed); }
  static constexpr size_t capacity() noexcept { return MAX_KEYS; }

  inline void clear() noexcept {
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      _values[i].store(nullptr, std::memory_order_relaxed);
      _keys[i].store(0, std::memory_order_relaxed);
      _ctrl[i].store(Control::Empty, std:: memory_order_relaxed);
    }
    for (size_t k = 0; k< SIMD_SIZE; ++k) {
      _ctrl[MAX_KEYS + k].store(Control::Empty, std::memory_order_relaxed);
    }
    _size.store(0, std:: memory_order_relaxed) ;
  }


  // Debug traversal
  // call f(pos, key, distance (unsuccessful atempts to insert a key) for each Live entry.
  // to support writing test cases
  template <typename Callback>
  inline void for_each(Callback && callback) const noexcept {
    for (size_t pos = 0; pos < MAX_KEYS; ++pos) {
      const int8_t c = _ctrl[pos].load(std::memory_order_acquire);
      if (c >= 0) {// Skip Empty/DeLeted/Busy
      const uint64_t key = _keys[pos].load(std::memory_order_acquire);
      Value* p = _values[pos].load(std::memory_order_acquire);
      if (p == nullptr) continue;
      const uint64_t h = detail::hash(key);
      const size_t idx = (h >> 7) & (MAX_KEYS - 1);
      size_t distance = (pos + MAX_KEYS - idx) & (MAX_KEYS - 1);
      callback (pos, key, distance);
      }
    }
  }

protected:
  inline void mirror_tail_relaxed_() noexcept {
    for (size_t k = 0; k < SIMD_SIZE; ++k)
     _ctrl[MAX_KEYS + k].store(_ctrl[k].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
  }

  inline void set_ctrl_(size_t pos, int8_t v) noexcept {
    _ctrl[pos].store(v, std::memory_order_release);
    if (pos < SIMD_SIZE)
      _ctrl[MAX_KEYS + pos].store(v, std::memory_order_release);
  }

protected:
  alignas (16) std::array<std::atomic<int8_t>, MAX_KEYS + SIMD_SIZE> _ctrl;
  std::array<std::atomic<uint64_t>, MAX_KEYS> _keys;
  std::array<std::atomic<Value *>, MAX_KEYS>_values;
  alignas(64) std::atomic<size_t> _size;
};

// compile-time selection
template <typename Value, size_t MAX_KEYS,
          ThreadSafetyPolicy ThreadSafety = ThreadSafetyPolicy::Multi,
          DuplicatePolicy Policy = DuplicatePolicy::Reject>
class Hashmap : public std::conditional_t<
                          ThreadSafety == ThreadSafetyPolicy::Multi,
                          HashmapMT<Value, MAX_KEYS, Policy>,
                          HashmapST<Value, MAX_KEYS, Policy>> {

  using Base = std::conditional_t<ThreadSafety == ThreadSafetyPolicy::Multi,
                                  HashmapMT<Value, MAX_KEYS, Policy>,
                                  HashmapST<Value, MAX_KEYS, Policy>>;
public:
  using Base:: Base;
};

}