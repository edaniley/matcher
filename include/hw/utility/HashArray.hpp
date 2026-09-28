#pragma once

#if !defined(__SSE2__)
  #error "This header requires SSE2 instructions to be enabled. Please use -msse2 or appropriate compiler flags."
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <cstring>
#ifdef DEBUG
	#include <iostream>
	#include <bitset>
#endif

#include <immintrin.h>

namespace hw::utility::swisstable {

// namespace detail {
//   // copied from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
//   static inline uint64_t hash64(uint64_t k) noexcept {
//     k ^= k >> 33;
//     k *= 0xff51afd7ed558ccdULL;
//     k ^= k >> 33;
//     k *= 0xc4ceb9fe1a85ec53ULL;
//     k ^= k >> 33;
//     k *= 0xc4ceb9fe1a85ec53ULL;
//     return k;
//   }
// }


static constexpr size_t SIMD_SIZE = 16;
enum class InsertResult { Success, DuplicateKey, TableFull };


//
// https://abseil.io/about/design/swisstables
// SwissTableHashmap: fixed-capacity hash map (key -> pointer payload)
// - No allocations after construction
// - Open addressing with linear probing
// - SIMD-accelerated group probing of 16 control bytes (SSE2)
//
// https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html#techs=SSE_ALL

template <typename KeyType, typename ValueType, size_t MAX_KEYS, bool THREAD_SAFE>
class HashArrayBase;

//
//  Single-Threaded Implementation
//
template <typename KeyType, typename ValueType, size_t MAX_KEYS>
class HashArrayBase<KeyType, ValueType, MAX_KEYS, false> {
  static_assert((MAX_KEYS & (MAX_KEYS - 1)) == 0, "MAX_KEYS must be a power of 2");
  static_assert(MAX_KEYS >= SIMD_SIZE, "MAX_KEYS must be at least 16 for SIMD probing");

#ifdef DEBUG
  static void print(__m128i value) {
    std::bitset<sizeof(value)*8> bs;
    std::memcpy ((void *)&bs, &value, sizeof(value));
    std::cout << bs.to_string() << std::endl;
  }
#endif

public:
  HashArrayBase() {
    _ctrl.fill(Control::Empty);
  }

  InsertResult insert(const KeyType & key, ValueType * value) noexcept {
    const uint64_t hashval = key.hash();
    const int8_t tag = static_cast<int8_t>(hashval & 0x7F);
    const size_t start_idx = (hashval >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; ++i) {
      const size_t idx = (start_idx + i) & (MAX_KEYS - 1);
      int8_t ctrl = _ctrl[idx];

      if (ctrl == tag) {
        if (_keys[idx] == key) {
          return InsertResult::DuplicateKey;
        }
      }
      else if (ctrl == Control::Empty) {
        _ctrl[idx] = tag;
        if (idx < SIMD_SIZE) {
          _ctrl[MAX_KEYS + idx] = tag; // update mirrored tail
        }
        _keys[idx] = key;
        _data[idx] = value;
        return InsertResult::Success;
      }
    }
    return InsertResult::TableFull;
  }

  ValueType * find(const KeyType & key) const noexcept {
    const uint64_t hashval = key.hash();
    const int8_t tag = static_cast<int8_t>(hashval & 0x7F);
    const size_t start_idx = (hashval >> 7) & (MAX_KEYS - 1);
    for (size_t i = 0; i < MAX_KEYS; i += SIMD_SIZE) {
      const size_t group_idx = (start_idx + i) & (MAX_KEYS - 1);
      // Always safe to load 16 bytes due to mirrored tail
      __m128i group = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&_ctrl[group_idx]));
      // _mm_cmpeq_epi8 intrinsic performs an element-wise comparison for equality on two 128-bit SIMD registers,
      //    treating them as vectors of 16 signed or unsigned 8-bit integers (bytes).
      // Input: It takes two arguments, both of type __m128i, which are 128-bit integer vectors stored in XMM registers.
      // Operation: It compares each of the 16 bytes in the first input vector with the corresponding byte in the second
      //    input vector simultaneously (in parallel).
      // Output: It returns a new __m128i vector of 16 bytes. For each element:
      //    If the corresponding bytes in the input vectors are equal, the resulting byte is set to OxFF (all bits are 1).
      //    If the corresponding bytes are not equal, the resulting byte is set to 0x00 (all bits are 0).
      const __m128i matchedBytes = _mm_cmpeq_epi8(group, _mm_set1_epi8(tag));
      //_mm_movemask_epi8 intrinsic is an SSE2 instruction that takes a 128-bit integer vector
      //    containing 16 separate 8-bit elements and produces a 16-bit integer (an int) where each bit is
      //    the most significant bit (MSB) of the corresponding byte in the input vector.
      // The function extracts the most significant bit (the sign bit) from each of the 16 bytes in
      //    the input __m128i register and packs these bits into a single 16-bit integer result.
      // Input: A 128-bit SIMD register (e. g., __m128i) that holds 16 individual 8-bit values (bytes).
      // Process: For each of the 16 bytes, the intrinsic checks the 8th bit (the most significant bit).
      // Output: An int (typically 32-bit on x86-64 platforms, but only the lower 16 bits are used) where:
      //    Bit 0 of the result is the MSB of the first (least significant) byte of the input vector.
      //    Bit 1 of the result is the MSB of the second byte.
      //    ...and so on, up to bit 15, which is the MSB of the sixteenth (most significant) byte.
      uint32_t matchedBits = _mm_movemask_epi8(matchedBytes);
      // print (matchedBytes);
      // print (matchedBits);
      while (matchedBits) {
        int bit = __builtin_ctz(matchedBits); // returns 1st set bit i.e. index of potential match
        size_t idx = (group_idx + bit) & (MAX_KEYS - 1);
        if (_keys[idx] == key) [[likely]] {
          return _data[idx];
        }
        matchedBits &= ~(1 << bit); // clears lowest set bit; try next if present
      }
      // early exit if nay byte in the group is Empty
      const uint32_t emptyMask = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(group, _mm_set1_epi8 (Control:: Empty))));
      if (emptyMask) [[likely]] return nullptr;
    }
    return nullptr;
  }

  template <typename Fn>
  void for_each(Fn &&f) const noexcept {
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      int8_t ctrl = _ctrl[i];
      if (ctrl != Control::Empty) {
        f(_keys[i], _data[i]);
      }
    }
  }

private:
  struct Control {
    static constexpr int8_t Empty = static_cast<int8_t>(0xFF);
  };

  alignas(SIMD_SIZE) std::array<int8_t, MAX_KEYS + SIMD_SIZE> _ctrl; // +SIMD_SIZE for Mirror
  std::array<KeyType, MAX_KEYS> _keys;
  std::array<ValueType*, MAX_KEYS> _data;
};

//
//  Thread-Safe Implementation
//
template <typename KeyType, typename ValueType, size_t MAX_KEYS>
class HashArrayBase<KeyType, ValueType, MAX_KEYS, true> {
  static_assert((MAX_KEYS & (MAX_KEYS - 1)) == 0, "MAX_KEYS must be a power of 2");
  static_assert(MAX_KEYS >= SIMD_SIZE, "MAX_KEYS must be at least 16 for SIMD probing");

public:
  HashArrayBase() {
    for (auto& ctrl : _ctrl) {
      ctrl.store(Control::Empty, std::memory_order_relaxed);
    }
    // why cannot we simply memset?
  }

  InsertResult insert(const KeyType & key, ValueType * value) noexcept {
    const uint64_t hashval = key.hash();
    const int8_t tag = static_cast<int8_t>(hashval & 0x7F);
    const size_t start_idx = (hashval >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; ++i) {
      const size_t idx = (start_idx + i) & (MAX_KEYS - 1);
      int8_t ctrl = _ctrl[idx].load(std::memory_order_acquire);

      if (ctrl == tag) {
        if (_keys[idx] == key) return InsertResult::DuplicateKey;
      }

      if (ctrl == Control::Empty) {
        if (_ctrl[idx].compare_exchange_strong(ctrl, Control::Busy,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) [[likely]] {
          _keys[idx] = key;
          _data[idx] = value;
          _ctrl[idx].store(tag, std::memory_order_release);
          return InsertResult::Success;
        }
        i--;
        continue; // retry
      }

      if (ctrl == Control::Busy) [[unlikely]] {
        _mm_pause();
        i--;
        continue; // retry
      }
    }
    return InsertResult::TableFull;
  }

  // we assume that if we are searhing for a key then it probably exists
  // TODO - add traits to optimize search vs insert
  ValueType * find(const KeyType & key) const noexcept {
    const uint64_t hashval = key.hash();
    const int8_t tag = static_cast<int8_t>(hashval & 0x7F);
    const size_t start_idx = (hashval >> 7) & (MAX_KEYS - 1);

    for (size_t i = 0; i < MAX_KEYS; i += SIMD_SIZE) {
      const size_t group_idx = (start_idx + i) & (MAX_KEYS - 1);

      if (group_idx + SIMD_SIZE <= MAX_KEYS) {
        const __m128i group = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&_ctrl[group_idx]));
        const __m128i matchedBytes = _mm_cmpeq_epi8(group, _mm_set1_epi8(tag));

        uint32_t matchedBits = _mm_movemask_epi8(matchedBytes);
        while (matchedBits) {
          int bit = __builtin_ctz(matchedBits);
          size_t idx = group_idx + bit;

          // confirm match
          if (_ctrl[idx].load(std::memory_order_acquire) == tag) {
            if (_keys[idx] == key) [[likely]] {
              return _data[idx];
            }
          }
          matchedBits &= ~(1 << bit); // clears lowest set bit; try next if present
        }

        const __m128i emptyMask = _mm_cmpeq_epi8(group, _mm_set1_epi8(Control::Empty));
        uint32_t emptyBits = _mm_movemask_epi8(emptyMask);
        if (emptyBits) {
          // Check all empty candidates with Acquire to confirm they are truly empty
          while (emptyBits) {
            int bit = __builtin_ctz(emptyBits);
            size_t idx = group_idx + bit;
            if (_ctrl[idx].load(std::memory_order_acquire) == Control::Empty) {
              return nullptr;
            }
            // If race caused it to not be empty, try next empty bit
            emptyBits &= ~(1 << bit);
          }
        }
      }
      else { // we are close to the end
        // we cannot maintain mirror group atomically
        // check byte by byte for wrap-around
        for (size_t k = 0; k < SIMD_SIZE; ++k) {
          size_t idx = (group_idx + k) & (MAX_KEYS - 1);
          int8_t ctrl = _ctrl[idx].load(std::memory_order_acquire);
          if (ctrl == tag && _keys[idx] == key) return _data[idx];
          if (ctrl == Control::Empty) return nullptr;
        }
       }
    }
    return nullptr;
  }

  template <typename Fn>
  void for_each(Fn &&f) const noexcept {
    for (size_t i = 0; i < MAX_KEYS; ++i) {
      int8_t ctrl = _ctrl[i].load(std::memory_order_acquire);
      if (ctrl != Control::Empty && ctrl != Control::Busy) {
        f(_keys[i], _data[i]);
      }
    }
  }

private:
  struct Control {
    static constexpr int8_t Empty = static_cast<int8_t>(0xFF);
    static constexpr int8_t Busy  = static_cast<int8_t>(0xFE);
  };

  alignas(SIMD_SIZE) std::array<std::atomic<int8_t>, MAX_KEYS> _ctrl;
  std::array<KeyType, MAX_KEYS> _keys;
  std::array<ValueType*, MAX_KEYS> _data;
};

// compile-time selection
template <typename KeyType, typename ValueType, size_t MAX_KEYS, bool THREAD_SAFE = true>
class HashArray : public HashArrayBase<KeyType, ValueType, MAX_KEYS, THREAD_SAFE> {
public:
  using Base = HashArrayBase<KeyType, ValueType, MAX_KEYS, THREAD_SAFE>;
  using Base::Base;
};


//
//  Generic Key Template for HashArray
//  Expects sizeof(Type) to match SIZE (for casting).
//  If SIZE >= SIMD_SIZE, assumes it might benefit from SIMD/memcmp optimizations.
//
template <size_t SIZE>
class Key {
public:
  // Helper to view raw bytes as Type
  template <typename Type>
  Type * data() noexcept { return reinterpret_cast<Type*>(_data); }

  template <typename Type>
  const Type * data() const noexcept { return reinterpret_cast<const Type*>(_data); }

  // Helper to view raw bytes directly
  std::byte * raw() noexcept { return _data; }
  const std::byte * raw() const noexcept { return _data; }

  uint64_t hash() const noexcept {
    // FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < SIZE; ++i) {
      hash ^= static_cast<uint8_t>(_data[i]);
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  bool operator == (const Key & other) const noexcept {
    return std::memcmp(_data, other._data, SIZE) == 0;
  }

private:
  alignas(8) std::byte _data[SIZE];
};

} // namespace hw::utility::swisstable
