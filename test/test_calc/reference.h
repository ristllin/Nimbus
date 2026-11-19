#pragma once
// The big-decimal reference for test_calc: unbounded integers and unreduced fractions,
// written independently of orch_calc.cpp (no shared code, a different representation and a
// different way to decide exactness: digit expansion instead of factoring the denominator).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ref {

// An unsigned integer of any size: 32-bit limbs, least significant first, no zero limbs on top.
struct Big {
  std::vector<uint32_t> m;
  bool zero() const { return m.empty(); }
  void trim() {
    while (!m.empty() && m.back() == 0) m.pop_back();
  }
  size_t bits() const {
    if (m.empty()) return 0;
    size_t b = (m.size() - 1) * 32;
    uint32_t top = m.back();
    while (top) {
      b++;
      top >>= 1;
    }
    return b;
  }
  bool bit(size_t i) const { return i / 32 < m.size() && ((m[i / 32] >> (i % 32)) & 1u); }
};

inline Big fromU64(uint64_t v) {
  Big b;
  while (v) {
    b.m.push_back((uint32_t)v);
    v >>= 32;
  }
  return b;
}

inline int cmp(const Big& a, const Big& b) {
  if (a.m.size() != b.m.size()) return a.m.size() < b.m.size() ? -1 : 1;
  for (size_t i = a.m.size(); i-- > 0;)
    if (a.m[i] != b.m[i]) return a.m[i] < b.m[i] ? -1 : 1;
  return 0;
}

inline Big add(const Big& a, const Big& b) {
  Big r;
  uint64_t carry = 0;
  for (size_t i = 0; i < a.m.size() || i < b.m.size() || carry; i++) {
    uint64_t s = carry;
    if (i < a.m.size()) s += a.m[i];
    if (i < b.m.size()) s += b.m[i];
    r.m.push_back((uint32_t)s);
    carry = s >> 32;
  }
  r.trim();
  return r;
}

// a - b for a >= b.
inline Big sub(const Big& a, const Big& b) {
  Big r;
  int64_t borrow = 0;
  for (size_t i = 0; i < a.m.size(); i++) {
    int64_t s = (int64_t)a.m[i] - borrow - (i < b.m.size() ? (int64_t)b.m[i] : 0);
    borrow = s < 0;
    if (s < 0) s += (int64_t)1 << 32;
    r.m.push_back((uint32_t)s);
  }
  r.trim();
  return r;
}

inline Big mul(const Big& a, const Big& b) {
  if (a.zero() || b.zero()) return Big{};
  std::vector<uint64_t> acc(a.m.size() + b.m.size() + 1, 0);
  for (size_t i = 0; i < a.m.size(); i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.m.size(); j++) {
      uint64_t cur = acc[i + j] + (uint64_t)a.m[i] * b.m[j] + carry;
      acc[i + j] = (uint32_t)cur;
      carry = cur >> 32;
    }
    size_t k = i + b.m.size();
    while (carry) {
      uint64_t cur = acc[k] + carry;
      acc[k] = (uint32_t)cur;
      carry = cur >> 32;
      k++;
    }
  }
  Big r;
  for (uint64_t v : acc) r.m.push_back((uint32_t)v);
  r.trim();
  return r;
}

// In place: a = a * 2 + low.
inline void shl1In(Big& a, bool low) {
  uint32_t carry = low ? 1u : 0u;
  for (uint32_t& v : a.m) {
    const uint32_t top = v >> 31;
    v = (v << 1) | carry;
    carry = top;
  }
  if (carry) a.m.push_back(carry);
}

// In place: a -= b for a >= b.
inline void subIn(Big& a, const Big& b) {
  int64_t borrow = 0;
  for (size_t i = 0; i < a.m.size(); i++) {
    int64_t s = (int64_t)a.m[i] - borrow - (i < b.m.size() ? (int64_t)b.m[i] : 0);
    borrow = s < 0;
    if (s < 0) s += (int64_t)1 << 32;
    a.m[i] = (uint32_t)s;
  }
  a.trim();
}

// Bit-by-bit long division: q = a / b, r = a % b (b nonzero).
inline void divmod(const Big& a, const Big& b, Big& q, Big& r) {
  r = Big{};
  const size_t n = a.bits();
  q.m.assign((n + 31) / 32, 0);
  for (size_t i = n; i-- > 0;) {
    shl1In(r, a.bit(i));
    r.trim();
    if (cmp(r, b) >= 0) {
      subIn(r, b);
      q.m[i / 32] |= 1u << (i % 32);
    }
  }
  q.trim();
}

// q = a / d, returns a % d, for a small divisor.
inline uint32_t divSmall(const Big& a, uint32_t d, Big& q) {
  q.m.assign(a.m.size(), 0);
  uint64_t rem = 0;
  for (size_t i = a.m.size(); i-- > 0;) {
    const uint64_t cur = (rem << 32) | a.m[i];
    q.m[i] = (uint32_t)(cur / d);
    rem = cur % d;
  }
  q.trim();
  return (uint32_t)rem;
}

inline size_t ctz(const Big& a) {
  size_t i = 0;
  while (!a.bit(i)) i++;
  return i;
}

inline void shrIn(Big& a, size_t k) {
  const size_t limbs = k / 32, bitsK = k % 32;
  if (limbs >= a.m.size()) {
    a.m.clear();
    return;
  }
  a.m.erase(a.m.begin(), a.m.begin() + (long)limbs);
  if (bitsK) {
    for (size_t i = 0; i < a.m.size(); i++) {
      const uint32_t hi = i + 1 < a.m.size() ? a.m[i + 1] : 0;
      a.m[i] = (a.m[i] >> bitsK) | (hi << (32 - bitsK));
    }
  }
  a.trim();
}

// Binary gcd.
inline Big gcd(Big a, Big b) {
  if (a.zero()) return b;
  if (b.zero()) return a;
  const size_t za = ctz(a), zb = ctz(b), shift = za < zb ? za : zb;
  shrIn(a, za);
  while (!b.zero()) {
    shrIn(b, ctz(b));
    if (cmp(a, b) > 0) std::swap(a, b);
    subIn(b, a);
  }
  for (size_t i = 0; i < shift; i++) shl1In(a, false);
  return a;
}

inline Big pow10(unsigned k) {
  Big r = fromU64(1);
  for (unsigned i = 0; i < k; i++) r = mul(r, fromU64(10));
  return r;
}

inline Big fromDigits(const std::string& digits) {
  Big r;
  for (char c : digits) r = add(mul(r, fromU64(10)), fromU64((uint64_t)(c - '0')));
  return r;
}

inline std::string toDigits(Big a) {
  if (a.zero()) return "0";
  std::string s;
  while (!a.zero()) {
    Big q;
    const uint32_t r = divSmall(a, 10, q);
    s.insert(s.begin(), (char)('0' + r));
    a = q;
  }
  return s;
}

// A signed fraction, never reduced (den > 0).
struct Rat {
  bool neg = false;
  Big num, den = fromU64(1);
};

inline Rat neg(Rat a) {
  if (!a.num.zero()) a.neg = !a.neg;
  return a;
}

inline Rat addR(const Rat& a, const Rat& b) {
  const Big x = mul(a.num, b.den), y = mul(b.num, a.den);
  Rat r;
  r.den = mul(a.den, b.den);
  if (a.neg == b.neg) {
    r.num = add(x, y);
    r.neg = a.neg;
  } else if (cmp(x, y) >= 0) {
    r.num = sub(x, y);
    r.neg = a.neg;
  } else {
    r.num = sub(y, x);
    r.neg = b.neg;
  }
  if (r.num.zero()) r.neg = false;
  return r;
}

inline Rat mulR(const Rat& a, const Rat& b) {
  Rat r;
  r.num = mul(a.num, b.num);
  r.den = mul(a.den, b.den);
  r.neg = !r.num.zero() && (a.neg != b.neg);
  return r;
}

inline Rat invR(const Rat& a) {
  Rat r;
  r.num = a.den;
  r.den = a.num;
  r.neg = a.neg;
  return r;
}

inline Rat reduce(const Rat& a) {
  Rat r = a;
  if (a.num.zero()) {
    r.den = fromU64(1);
    return r;
  }
  const Big g = gcd(a.num, a.den);
  Big rem;
  divmod(a.num, g, r.num, rem);
  divmod(a.den, g, r.den, rem);
  return r;
}

// The expected result line, by decimal expansion: exact when the expansion ends within 12
// places, otherwise the first 7 places decide a half-away-from-zero rounding to 6.
inline std::string render(const Rat& v) {
  Big q, r;
  divmod(v.num, v.den, q, r);
  std::string frac;
  const Big ten = fromU64(10);
  while (!r.zero() && frac.size() < 13) {
    r = mul(r, ten);
    char digit = '0';
    while (cmp(r, v.den) >= 0) {
      subIn(r, v.den);
      digit++;
    }
    frac.push_back(digit);
  }
  const bool exact = r.zero() && frac.size() <= 12;
  std::string ip = toDigits(q);
  if (!exact) {
    const bool up = frac.size() > 6 && frac[6] >= '5';
    frac.resize(6, '0');
    if (up) {
      std::string all = ip + frac;
      size_t i = all.size();
      while (i-- > 0) {
        if (all[i] == '9') {
          all[i] = '0';
          continue;
        }
        all[i]++;
        break;
      }
      if (i == (size_t)-1) all.insert(all.begin(), '1');
      ip = all.substr(0, all.size() - 6);
      frac = all.substr(all.size() - 6);
    }
  }
  while (!frac.empty() && frac.back() == '0') frac.pop_back();
  std::string out = "= ";
  if (v.neg && !(ip == "0" && frac.empty())) out += "-";
  out += ip;
  if (!frac.empty()) out += "." + frac;
  if (!exact) out += " (rounded to 6 decimal places)";
  return out;
}

}  // namespace ref
