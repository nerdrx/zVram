#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace zvram {
namespace detail {

inline void shuffle2(const std::uint8_t* src, std::uint8_t* dst,
                     std::size_t size) noexcept {
  const std::size_t groups = size / 2;
  std::size_t done = 0;
#if defined(__SSE2__)
  const __m128i mask = _mm_set1_epi16(0x00ff);
  for (; done + 16 <= groups; done += 16) {
    const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 2));
    const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 2 + 16));
    const auto low = _mm_packus_epi16(_mm_and_si128(a, mask), _mm_and_si128(b, mask));
    const auto high = _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done), low);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + groups + done), high);
  }
#endif
  for (; done < groups; ++done) {
    dst[done] = src[done * 2];
    dst[groups + done] = src[done * 2 + 1];
  }
  const std::size_t aligned = groups * 2;
  std::memcpy(dst + aligned, src + aligned, size - aligned);
}

inline void unshuffle2(const std::uint8_t* src, std::uint8_t* dst,
                       std::size_t size) noexcept {
  const std::size_t groups = size / 2;
  std::size_t done = 0;
#if defined(__SSE2__)
  for (; done + 16 <= groups; done += 16) {
    const auto low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done));
    const auto high = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + groups + done));
    const auto a = _mm_unpacklo_epi8(low, high);
    const auto b = _mm_unpackhi_epi8(low, high);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 2), a);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 2 + 16), b);
  }
#endif
  for (; done < groups; ++done) {
    dst[done * 2] = src[done];
    dst[done * 2 + 1] = src[groups + done];
  }
  const std::size_t aligned = groups * 2;
  std::memcpy(dst + aligned, src + aligned, size - aligned);
}

inline void shuffle4(const std::uint8_t* src, std::uint8_t* dst,
                     std::size_t size) noexcept {
  const std::size_t groups = size / 4;
  std::size_t done = 0;
#if defined(__SSE2__)
  const __m128i mask = _mm_set1_epi32(0xff);
  for (; done + 16 <= groups; done += 16) {
    const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 4));
    const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 4 + 16));
    const auto c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 4 + 32));
    const auto d = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done * 4 + 48));
    for (unsigned plane = 0; plane < 4; ++plane) {
      const auto shift = _mm_cvtsi32_si128(static_cast<int>(plane * 8));
      const auto x0 = _mm_and_si128(_mm_srl_epi32(a, shift), mask);
      const auto x1 = _mm_and_si128(_mm_srl_epi32(b, shift), mask);
      const auto x2 = _mm_and_si128(_mm_srl_epi32(c, shift), mask);
      const auto x3 = _mm_and_si128(_mm_srl_epi32(d, shift), mask);
      const auto lo = _mm_packs_epi32(x0, x1);
      const auto hi = _mm_packs_epi32(x2, x3);
      const auto bytes = _mm_packus_epi16(lo, hi);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + plane * groups + done), bytes);
    }
  }
#endif
  for (; done < groups; ++done)
    for (std::size_t plane = 0; plane < 4; ++plane)
      dst[plane * groups + done] = src[done * 4 + plane];
  const std::size_t aligned = groups * 4;
  std::memcpy(dst + aligned, src + aligned, size - aligned);
}

inline void unshuffle4(const std::uint8_t* src, std::uint8_t* dst,
                       std::size_t size) noexcept {
  const std::size_t groups = size / 4;
  std::size_t done = 0;
#if defined(__SSE2__)
  for (; done + 16 <= groups; done += 16) {
    const auto p0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + done));
    const auto p1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + groups + done));
    const auto p2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 2 * groups + done));
    const auto p3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + 3 * groups + done));
    const auto a = _mm_unpacklo_epi8(p0, p1);
    const auto b = _mm_unpackhi_epi8(p0, p1);
    const auto c = _mm_unpacklo_epi8(p2, p3);
    const auto d = _mm_unpackhi_epi8(p2, p3);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 4), _mm_unpacklo_epi16(a, c));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 4 + 16), _mm_unpackhi_epi16(a, c));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 4 + 32), _mm_unpacklo_epi16(b, d));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + done * 4 + 48), _mm_unpackhi_epi16(b, d));
  }
#endif
  for (; done < groups; ++done)
    for (std::size_t plane = 0; plane < 4; ++plane)
      dst[done * 4 + plane] = src[plane * groups + done];
  const std::size_t aligned = groups * 4;
  std::memcpy(dst + aligned, src + aligned, size - aligned);
}

}  // namespace detail

// Input and output must be disjoint buffers of at least size bytes.
// Unsupported strides or null non-empty buffers return false.
inline bool byte_shuffle(const std::uint8_t* src, std::uint8_t* dst,
                         std::size_t size, unsigned stride) noexcept {
  if (stride != 2 && stride != 4) return false;
  if (size == 0) return true;
  if (!src || !dst || src == dst) return false;
  if (stride == 2) detail::shuffle2(src, dst, size);
  else detail::shuffle4(src, dst, size);
  return true;
}

inline bool byte_unshuffle(const std::uint8_t* src, std::uint8_t* dst,
                           std::size_t size, unsigned stride) noexcept {
  if (stride != 2 && stride != 4) return false;
  if (size == 0) return true;
  if (!src || !dst || src == dst) return false;
  if (stride == 2) detail::unshuffle2(src, dst, size);
  else detail::unshuffle4(src, dst, size);
  return true;
}

}  // namespace zvram
