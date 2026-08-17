#include "nimbus/qr.h"

#include <cstdlib>
#include <cstring>
#include <vector>

// Byte-mode QR encoder (ECC level M, versions 1-6): a compact adaptation of
// Project Nayuki's QR-Code-generator library ("qrcodegen"),
//   https://www.nayuki.io/page/qr-code-generator-library
//   Copyright (c) Project Nayuki - MIT License (full text in the repo NOTICE).
// The GF(256)/Reed-Solomon math, function-pattern layout, zigzag codeword
// placement, format-bits BCH and the full penalty-based mask selection are
// algorithmically identical to the reference, so the known-answer module maps
// in test/test_qr/qr_vectors.h (generated from Nayuki's reference Python
// qrcodegen) must - and do - agree bit-for-bit. Trimmed relative to the
// reference: byte mode only, ECC M only, v1-6 (no version-info block, which
// only exists from v7), no ECC boosting.

namespace nimbus::qr {

namespace {

// Per-version tables for ECC level M, index = version - 1 (ISO 18004 tables,
// matching the reference's ECC_CODEWORDS_PER_BLOCK / NUM_ERROR_CORRECTION_BLOCKS).
constexpr int kMinVersion = 1;
constexpr int kMaxVersion = 6;
constexpr int kTotalCodewords[kMaxVersion] = {26, 44, 70, 100, 134, 172};
constexpr int kEccPerBlock[kMaxVersion]    = {10, 16, 26, 18, 24, 16};
constexpr int kNumBlocks[kMaxVersion]      = {1, 1, 1, 2, 2, 4};

// Mask-evaluation penalty weights (ISO 18004 N1..N4).
constexpr int kPenaltyN1 = 3;
constexpr int kPenaltyN2 = 3;
constexpr int kPenaltyN3 = 40;
constexpr int kPenaltyN4 = 10;

int dataCodewords(int ver) {
  return kTotalCodewords[ver - 1] - kEccPerBlock[ver - 1] * kNumBlocks[ver - 1];
}

// ---- GF(256) / Reed-Solomon (reducing polynomial x^8+x^4+x^3+x^2+1) --------

uint8_t gfMul(uint8_t x, uint8_t y) {
  int z = 0;
  for (int i = 7; i >= 0; --i) {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return uint8_t(z);
}

// Generator polynomial (x - r^0)(x - r^1)...(x - r^{degree-1}), r = 0x02.
std::vector<uint8_t> rsComputeDivisor(int degree) {
  std::vector<uint8_t> result(size_t(degree), 0);
  result[size_t(degree - 1)] = 1;  // start with the monomial x^0
  uint8_t root = 1;
  for (int i = 0; i < degree; ++i) {
    for (int j = 0; j < degree; ++j) {
      result[size_t(j)] = gfMul(result[size_t(j)], root);
      if (j + 1 < degree) result[size_t(j)] ^= result[size_t(j + 1)];
    }
    root = gfMul(root, 0x02);
  }
  return result;
}

std::vector<uint8_t> rsComputeRemainder(const std::vector<uint8_t>& data,
                                        const std::vector<uint8_t>& divisor) {
  std::vector<uint8_t> result(divisor.size(), 0);
  for (uint8_t b : data) {  // polynomial division
    const uint8_t factor = b ^ result[0];
    result.erase(result.begin());
    result.push_back(0);
    for (size_t i = 0; i < divisor.size(); ++i)
      result[i] ^= gfMul(divisor[i], factor);
  }
  return result;
}

// Split into RS blocks, compute ECC per block, interleave into the final
// codeword sequence. General short/long-block form kept for fidelity (for
// v1-6 at M every block happens to be the same length).
std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data, int ver) {
  const int numBlocks = kNumBlocks[ver - 1];
  const int blockEccLen = kEccPerBlock[ver - 1];
  const int rawCodewords = kTotalCodewords[ver - 1];
  const int numShortBlocks = numBlocks - rawCodewords % numBlocks;
  const int shortBlockLen = rawCodewords / numBlocks;

  std::vector<std::vector<uint8_t>> blocks;
  const std::vector<uint8_t> rsDiv = rsComputeDivisor(blockEccLen);
  for (int i = 0, k = 0; i < numBlocks; ++i) {
    std::vector<uint8_t> dat(
        data.begin() + k,
        data.begin() + (k + shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1)));
    k += int(dat.size());
    const std::vector<uint8_t> ecc = rsComputeRemainder(dat, rsDiv);
    if (i < numShortBlocks) dat.push_back(0);  // pad placeholder, skipped below
    dat.insert(dat.end(), ecc.begin(), ecc.end());
    blocks.push_back(std::move(dat));
  }

  std::vector<uint8_t> result;
  result.reserve(size_t(rawCodewords));
  for (size_t i = 0; i < blocks[0].size(); ++i) {
    for (size_t j = 0; j < blocks.size(); ++j) {
      if (i != size_t(shortBlockLen - blockEccLen) || j >= size_t(numShortBlocks))
        result.push_back(blocks[j][i]);
    }
  }
  return result;
}

// ---- module grid ------------------------------------------------------------

// Working canvas: one byte per module for `mod` (dark) and `fun` (function-
// pattern reservation), plus every drawing/masking/penalty step. Scratch only;
// encode() packs the result into QrCode::modules at the end.
struct Grid {
  int size;
  std::vector<uint8_t> mod, fun;

  explicit Grid(int sz)
      : size(sz), mod(size_t(sz) * size_t(sz), 0), fun(size_t(sz) * size_t(sz), 0) {}

  bool get(int x, int y) const { return mod[size_t(y) * size_t(size) + size_t(x)] != 0; }
  void set(int x, int y, bool dark) { mod[size_t(y) * size_t(size) + size_t(x)] = dark; }
  bool isFunction(int x, int y) const { return fun[size_t(y) * size_t(size) + size_t(x)] != 0; }
  void setFunction(int x, int y, bool dark) {
    set(x, y, dark);
    fun[size_t(y) * size_t(size) + size_t(x)] = 1;
  }

  // 7x7 finder centred at (cx,cy); clipped at the edges like the reference.
  void drawFinderPattern(int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy) {
      for (int dx = -4; dx <= 4; ++dx) {
        const int dist = std::abs(dx) > std::abs(dy) ? std::abs(dx) : std::abs(dy);
        const int x = cx + dx, y = cy + dy;
        if (0 <= x && x < size && 0 <= y && y < size)
          setFunction(x, y, dist != 2 && dist != 4);
      }
    }
  }

  // 5x5 alignment pattern centred at (cx,cy).
  void drawAlignmentPattern(int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
      for (int dx = -2; dx <= 2; ++dx)
        setFunction(cx + dx, cy + dy,
                    (std::abs(dx) > std::abs(dy) ? std::abs(dx) : std::abs(dy)) != 1);
  }

  // Format info: 5 data bits (ECC-M bits 00 + mask), BCH(15,5) remainder,
  // XOR mask 0x5412; drawn in both fixed copies. Also the always-dark module.
  void drawFormatBits(int msk) {
    const int data = (0 /* ECC M */ << 3) | msk;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;

    auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) setFunction(8, i, bit(i));  // first copy
    setFunction(8, 7, bit(6));
    setFunction(8, 8, bit(7));
    setFunction(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) setFunction(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) setFunction(size - 1 - i, 8, bit(i));  // second copy
    for (int i = 8; i < 15; ++i) setFunction(8, size - 15 + i, bit(i));
    setFunction(8, size - 8, true);  // always dark
  }

  void drawFunctionPatterns(int ver) {
    for (int i = 0; i < size; ++i) {  // timing patterns
      setFunction(6, i, i % 2 == 0);
      setFunction(i, 6, i % 2 == 0);
    }
    drawFinderPattern(3, 3);
    drawFinderPattern(size - 4, 3);
    drawFinderPattern(3, size - 4);
    if (ver >= 2) {  // v2-6 have exactly one free alignment pattern
      drawAlignmentPattern(size - 7, size - 7);
    }
    drawFormatBits(0);  // reserve the format areas (real bits drawn per mask)
  }

  // Zigzag placement of the interleaved codewords over non-function modules.
  void drawCodewords(const std::vector<uint8_t>& data) {
    size_t i = 0;  // bit index into data
    for (int right = size - 1; right >= 1; right -= 2) {
      if (right == 6) right = 5;  // skip the vertical timing column
      for (int vert = 0; vert < size; ++vert) {
        for (int j = 0; j < 2; ++j) {
          const int x = right - j;
          const bool upward = ((right + 1) & 2) == 0;
          const int y = upward ? size - 1 - vert : vert;
          if (!isFunction(x, y) && i < data.size() * 8) {
            set(x, y, ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0);
            ++i;
          }
        }
      }
    }
  }

  // XOR one of the eight mask patterns over the non-function modules.
  // Self-inverse: applying the same mask twice restores the grid.
  void applyMask(int msk) {
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        bool invert = false;
        switch (msk) {
          case 0: invert = (x + y) % 2 == 0; break;
          case 1: invert = y % 2 == 0; break;
          case 2: invert = x % 3 == 0; break;
          case 3: invert = (x + y) % 3 == 0; break;
          case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
          case 5: invert = x * y % 2 + x * y % 3 == 0; break;
          case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
          case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (invert && !isFunction(x, y)) set(x, y, !get(x, y));
      }
    }
  }

  // ---- penalty scoring (identical run-history method to the reference) ----

  // Push one finished run length onto the history; the border counts as an
  // extra `size` of light on the very first run.
  void finderPenaltyAddHistory(int runLength, int hist[7]) const {
    if (hist[0] == 0) runLength += size;
    std::memmove(&hist[1], &hist[0], 6 * sizeof(hist[0]));
    hist[0] = runLength;
  }

  // Count 1:1:3:1:1 (dark) patterns with >=4x light on either side.
  int finderPenaltyCountPatterns(const int hist[7]) const {
    const int n = hist[1];
    const bool core =
        n > 0 && hist[2] == n && hist[3] == n * 3 && hist[4] == n && hist[5] == n;
    return (core && hist[0] >= n * 4 && hist[6] >= n ? 1 : 0) +
           (core && hist[6] >= n * 4 && hist[0] >= n ? 1 : 0);
  }

  int finderPenaltyTerminateAndCount(bool curColor, int curLen, int hist[7]) const {
    if (curColor) {  // terminate the dark run
      finderPenaltyAddHistory(curLen, hist);
      curLen = 0;
    }
    curLen += size;  // light border after the final run
    finderPenaltyAddHistory(curLen, hist);
    return finderPenaltyCountPatterns(hist);
  }

  long getPenaltyScore() const {
    long result = 0;

    // Rows: same-colour runs (N1) + finder-like patterns (N3).
    for (int y = 0; y < size; ++y) {
      bool runColor = false;
      int runX = 0;
      int hist[7] = {};
      for (int x = 0; x < size; ++x) {
        if (get(x, y) == runColor) {
          ++runX;
          if (runX == 5) result += kPenaltyN1;
          else if (runX > 5) ++result;
        } else {
          finderPenaltyAddHistory(runX, hist);
          if (!runColor) result += finderPenaltyCountPatterns(hist) * kPenaltyN3;
          runColor = get(x, y);
          runX = 1;
        }
      }
      result += finderPenaltyTerminateAndCount(runColor, runX, hist) * kPenaltyN3;
    }
    // Columns: same again, transposed.
    for (int x = 0; x < size; ++x) {
      bool runColor = false;
      int runY = 0;
      int hist[7] = {};
      for (int y = 0; y < size; ++y) {
        if (get(x, y) == runColor) {
          ++runY;
          if (runY == 5) result += kPenaltyN1;
          else if (runY > 5) ++result;
        } else {
          finderPenaltyAddHistory(runY, hist);
          if (!runColor) result += finderPenaltyCountPatterns(hist) * kPenaltyN3;
          runColor = get(x, y);
          runY = 1;
        }
      }
      result += finderPenaltyTerminateAndCount(runColor, runY, hist) * kPenaltyN3;
    }

    // 2x2 blocks of one colour (N2).
    for (int y = 0; y < size - 1; ++y) {
      for (int x = 0; x < size - 1; ++x) {
        const bool c = get(x, y);
        if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1))
          result += kPenaltyN2;
      }
    }

    // Dark/light balance (N4).
    long dark = 0;
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x)
        if (get(x, y)) ++dark;
    const long total = long(size) * size;
    const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * kPenaltyN4;
    return result;
  }
};

}  // namespace

bool QrCode::module(int x, int y) const {
  if (x < 0 || y < 0 || x >= size || y >= size) return false;
  const int i = y * size + x;
  return ((modules[i >> 3] >> (7 - (i & 7))) & 1) != 0;
}

bool encode(const std::string& text, QrCode& out) {
  out = QrCode{};
  const size_t n = text.size();
  if (n == 0) return false;

  // Pick the smallest version whose data capacity holds mode(4) + count(8) +
  // payload bits. Byte counts are 8-bit for v1-9, so no per-version resizing.
  const size_t neededBits = 4 + 8 + 8 * n;
  int ver = 0;
  for (int v = kMinVersion; v <= kMaxVersion; ++v) {
    if (size_t(dataCodewords(v)) * 8 >= neededBits) { ver = v; break; }
  }
  if (ver == 0) return false;  // 107+ bytes exceeds v6-M

  // Bit stream: segment, terminator (<=4 zero bits), pad to a byte boundary,
  // then alternating 0xEC/0x11 pad codewords up to the version's capacity.
  const size_t capBits = size_t(dataCodewords(ver)) * 8;
  std::vector<bool> bb;
  bb.reserve(capBits);
  auto appendBits = [&bb](uint32_t val, int len) {
    for (int i = len - 1; i >= 0; --i) bb.push_back(((val >> i) & 1) != 0);
  };
  appendBits(0x4, 4);  // byte-mode indicator
  appendBits(uint32_t(n), 8);
  for (const char c : text) appendBits(uint8_t(c), 8);
  appendBits(0, int(capBits - bb.size() < 4 ? capBits - bb.size() : 4));
  appendBits(0, int((8 - bb.size() % 8) % 8));
  for (uint8_t pad = 0xEC; bb.size() < capBits; pad ^= 0xEC ^ 0x11)
    appendBits(pad, 8);

  std::vector<uint8_t> data(bb.size() / 8, 0);
  for (size_t i = 0; i < bb.size(); ++i)
    if (bb[i]) data[i >> 3] |= uint8_t(0x80u >> (i & 7));

  // Assemble the symbol.
  Grid g(17 + 4 * ver);
  g.drawFunctionPatterns(ver);
  g.drawCodewords(addEccAndInterleave(data, ver));

  // Try all eight masks, keep the lowest penalty (applyMask is self-inverse,
  // so each candidate is undone before the next trial).
  int best = 0;
  long minPenalty = -1;
  for (int msk = 0; msk < 8; ++msk) {
    g.applyMask(msk);
    g.drawFormatBits(msk);
    const long penalty = g.getPenaltyScore();
    if (minPenalty < 0 || penalty < minPenalty) {
      best = msk;
      minPenalty = penalty;
    }
    g.applyMask(msk);  // undo
  }
  g.applyMask(best);
  g.drawFormatBits(best);

  out.version = ver;
  out.size = g.size;
  for (int y = 0; y < g.size; ++y) {
    for (int x = 0; x < g.size; ++x) {
      if (g.get(x, y)) {
        const int i = y * g.size + x;
        out.modules[i >> 3] |= uint8_t(0x80u >> (i & 7));
      }
    }
  }
  return true;
}

}  // namespace nimbus::qr
