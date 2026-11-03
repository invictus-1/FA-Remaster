// Keyed (counter-based) random numbers for parallel order (docs/parallel-mode.md 4, 4.1): a draw is a pure function of
// its key, (session seed, the drawing entity's creation handle, the beat, the purpose, the n-th draw of that entity in
// the beat), not of how many draws came before it in the whole sim. So two units may draw on two threads, in either
// order, and get the same numbers as on one thread. No state between calls.
//
// The mixing is SplitMix64's finaliser (Steele, Lea, Flood 2014; the constants of its published form), applied to the
// key's parts in turn. Only +, ^, >>, * on 64-bit integers: the same bits on every compiler and platform.
#pragma once

#include <cstdint>

namespace moho {

inline uint64_t KeyedMix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// 32 random bits for (seed, serial, beat, purpose, n)
inline uint32_t KeyedRand32(uint64_t seed, uint64_t serial, uint32_t beat, uint32_t purpose, uint32_t n) {
  uint64_t h = KeyedMix64(seed ^ 0x5eed5eed5eed5eedull);
  h = KeyedMix64(h ^ serial);
  h = KeyedMix64(h ^ ((static_cast<uint64_t>(beat) << 32) | purpose));
  h = KeyedMix64(h ^ n);
  return static_cast<uint32_t>(h >> 32);
}

}  // namespace moho
