#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "byte_shuffle.hpp"

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

static Bytes read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static bool write_file(const std::string& path, const Bytes& data,
                       std::size_t size) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(size));
  return file.good();
}

static void reference_shuffle(const Bytes& src, Bytes& dst, std::size_t stride) {
  const std::size_t groups = src.size() / stride;
  const std::size_t aligned = groups * stride;
  for (std::size_t group = 0; group < groups; ++group)
    for (std::size_t lane = 0; lane < stride; ++lane)
      dst[lane * groups + group] = src[group * stride + lane];
  std::copy(src.begin() + aligned, src.end(), dst.begin() + aligned);
}

static void reference_unshuffle(const Bytes& src, Bytes& dst, std::size_t stride) {
  const std::size_t groups = src.size() / stride;
  const std::size_t aligned = groups * stride;
  for (std::size_t group = 0; group < groups; ++group)
    for (std::size_t lane = 0; lane < stride; ++lane)
      dst[group * stride + lane] = src[lane * groups + group];
  std::copy(src.begin() + aligned, src.end(), dst.begin() + aligned);
}

static bool check_shuffle_implementation() {
  std::vector<std::size_t> sizes;
  for (std::size_t n = 0; n <= 257; ++n) sizes.push_back(n);
  for (std::size_t n : {511u, 512u, 513u, 1023u, 1024u, 1025u,
                        4095u, 4096u, 4097u, 65535u, 65536u, 65537u})
    sizes.push_back(n);
  std::mt19937 rng(0x5a17u);
  std::size_t cases = 0;
  for (const auto size : sizes) {
    for (unsigned pattern = 0; pattern < 2; ++pattern) {
      Bytes input(size), expected(size), actual(size), restored(size);
      if (pattern == 0) std::fill(input.begin(), input.end(), 0);
      else for (auto& byte : input) byte = static_cast<std::uint8_t>(rng());
      for (const unsigned stride : {2u, 4u}) {
        reference_shuffle(input, expected, stride);
        if (!zvram::byte_shuffle(input.data(), actual.data(), size, stride) ||
            actual != expected ||
            !zvram::byte_unshuffle(actual.data(), restored.data(), size, stride) ||
            restored != input) {
          std::cerr << "shuffle equivalence/roundtrip failed at size=" << size
                    << " stride=" << stride << " pattern=" << pattern << "\n";
          return false;
        }
        Bytes expected_restore(size);
        reference_unshuffle(actual, expected_restore, stride);
        if (expected_restore != input) return false;
        ++cases;
      }
    }
  }
  const std::uint8_t byte = 7;
  std::uint8_t out = 0;
  std::uint8_t same = 7;
  if (zvram::byte_shuffle(&byte, &out, 1, 3) ||
      zvram::byte_unshuffle(&byte, &out, 1, 3) ||
      zvram::byte_shuffle(&byte, nullptr, 1, 2) ||
      zvram::byte_unshuffle(nullptr, &out, 1, 2) ||
      zvram::byte_shuffle(&same, &same, 1, 2) ||
      zvram::byte_unshuffle(&same, &same, 1, 4) ||
      !zvram::byte_shuffle(nullptr, nullptr, 0, 2) ||
      !zvram::byte_unshuffle(nullptr, nullptr, 0, 4))
    return false;
#if defined(__SSE2__)
  constexpr const char* backend = "sse2";
#else
  constexpr const char* backend = "portable";
#endif
  std::cout << "correctness=PASS cases=" << cases << " backend=" << backend << "\n";
  return true;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: cpu_zstd_shuffle_bench INPUT.raw\n";
    return 2;
  }
  const Bytes input = read_file(argv[1]);
  if (input.size() != 32u * 1024u * 1024u) {
    std::cerr << "expected exactly 32 MiB input\n";
    return 2;
  }
  if (!check_shuffle_implementation()) return 1;

  const Bytes tail_probe = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  for (const std::size_t stride : {2u, 4u}) {
    Bytes permuted(tail_probe.size()), restored(tail_probe.size());
    if (!zvram::byte_shuffle(tail_probe.data(), permuted.data(), tail_probe.size(), stride) ||
        !zvram::byte_unshuffle(permuted.data(), restored.data(), tail_probe.size(), stride))
      return 1;
    if (restored != tail_probe) {
      std::cerr << "tail-preserving shuffle self-check failed for stride " << stride << "\n";
      return 1;
    }
  }

  {
    Bytes encoded(ZSTD_compressBound(input.size())), decoded(input.size());
    std::size_t encoded_size = 0;
    for (int rep = 0; rep < 3; ++rep) {
      const auto pipeline_start = Clock::now();
      auto start = Clock::now();
      const auto enc = ZSTD_compress(encoded.data(), encoded.size(),
                                     input.data(), input.size(), 1);
      auto end = Clock::now();
      if (ZSTD_isError(enc)) {
        std::cerr << "baseline Zstd encode failed: " << ZSTD_getErrorName(enc) << "\n";
        return 1;
      }
      encoded_size = enc;
      const double encode_ms =
          std::chrono::duration<double, std::milli>(end - start).count();
      start = Clock::now();
      const auto dec = ZSTD_decompress(decoded.data(), decoded.size(),
                                       encoded.data(), encoded_size);
      end = Clock::now();
      if (ZSTD_isError(dec) || dec != input.size() || decoded != input) {
        std::cerr << "baseline Zstd exact roundtrip failed\n";
        return 1;
      }
      const double decode_ms =
          std::chrono::duration<double, std::milli>(end - start).count();
      const double full_ms =
          std::chrono::duration<double, std::milli>(end - pipeline_start).count();
      std::cout << "stride=0 rep=" << rep + 1
                << " shuffle_ms=0 encode_ms=" << encode_ms
                << " decode_ms=" << decode_ms << " unshuffle_ms=0"
                << " full_ms=" << full_ms << " encoded_bytes=" << encoded_size
                << " exact_roundtrip=PASS\n";
    }
  }

  for (const std::size_t stride : {2u, 4u}) {
    Bytes transformed(input.size()), decoded(input.size()), restored(input.size());
    Bytes encoded(ZSTD_compressBound(input.size()));
    std::size_t encoded_size = 0;
    const std::string tag = "stride" + std::to_string(stride);
    for (int rep = 0; rep < 3; ++rep) {
      const auto pipeline_start = Clock::now();
      auto start = Clock::now();
      if (!zvram::byte_shuffle(input.data(), transformed.data(), input.size(), stride))
        return 1;
      auto end = Clock::now();
      const double shuffle_ms =
          std::chrono::duration<double, std::milli>(end - start).count();

      start = Clock::now();
      const auto enc = ZSTD_compress(encoded.data(), encoded.size(),
                                     transformed.data(), transformed.size(), 1);
      end = Clock::now();
      if (ZSTD_isError(enc)) {
        std::cerr << "Zstd encode failed: " << ZSTD_getErrorName(enc) << "\n";
        return 1;
      }
      encoded_size = enc;
      const double encode_ms =
          std::chrono::duration<double, std::milli>(end - start).count();

      start = Clock::now();
      const auto dec = ZSTD_decompress(decoded.data(), decoded.size(),
                                       encoded.data(), encoded_size);
      end = Clock::now();
      if (ZSTD_isError(dec) || dec != transformed.size() || decoded != transformed) {
        std::cerr << "Zstd decode mismatch for " << tag << " rep " << rep + 1 << "\n";
        return 1;
      }
      const double decode_ms =
          std::chrono::duration<double, std::milli>(end - start).count();

      start = Clock::now();
      if (!zvram::byte_unshuffle(decoded.data(), restored.data(), input.size(), stride))
        return 1;
      end = Clock::now();
      const double unshuffle_ms =
          std::chrono::duration<double, std::milli>(end - start).count();
      const double full_ms =
          std::chrono::duration<double, std::milli>(end - pipeline_start).count();
      if (restored != input) {
        std::cerr << "unshuffle byte mismatch for " << tag << " rep " << rep + 1 << "\n";
        return 1;
      }
      std::cout << "stride=" << stride << " rep=" << rep + 1
                << " shuffle_ms=" << shuffle_ms << " encode_ms=" << encode_ms
                << " decode_ms=" << decode_ms << " unshuffle_ms=" << unshuffle_ms
                << " full_ms=" << full_ms << " encoded_bytes=" << encoded_size
                << " exact_roundtrip=PASS\n";
    }
    if (!write_file("internlm-f16-32m-offset64m-" + tag + ".zstd",
                    encoded, encoded_size)) {
      std::cerr << "failed writing compressed result for " << tag << "\n";
      return 1;
    }
  }
  std::cout << "input_bytes=" << input.size() << " zstd_level=1 workers=1"
            << " zstd_version=" << ZSTD_versionString()
            << " trailing_bytes_policy=preserve_unmodified\n";
  return 0;
}
