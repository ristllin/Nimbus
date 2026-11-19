// calc: exact decimal arithmetic on one expression (calc.h has the contract).
//
// A single pass: a recursive-descent parser that evaluates as it reads, over reduced
// fractions of unsigned 128-bit integers built from two 64-bit halves (the device's 32-bit
// target has no compiler 128-bit type). Everything lives on the stack; the only recursion is
// at '(' and stops at kMaxDepth.
#include "nimbus/orch/calc.h"

#include <cstring>

namespace nimbus::orch::calc {
namespace {

// ---- unsigned 128-bit integers -----------------------------------------------------------
struct U128 {
  uint64_t hi = 0, lo = 0;
};

constexpr U128 u128(uint64_t v) { return U128{0, v}; }
bool isZero(const U128& a) { return (a.hi | a.lo) == 0; }

int cmp(const U128& a, const U128& b) {
  if (a.hi != b.hi) return a.hi < b.hi ? -1 : 1;
  if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
  return 0;
}

// a + b; false when the sum needs more than 128 bits.
bool add(const U128& a, const U128& b, U128& out) {
  uint64_t lo = 0, hi = 0;
  const bool c0 = __builtin_add_overflow(a.lo, b.lo, &lo);
  const bool c1 = __builtin_add_overflow(a.hi, b.hi, &hi);
  const bool c2 = __builtin_add_overflow(hi, (uint64_t)c0, &hi);
  out = U128{hi, lo};
  return !(c1 || c2);
}

// a - b, modulo 2^128 (callers keep a >= b, or want the wrapped difference).
U128 sub(const U128& a, const U128& b) {
  const uint64_t borrow = a.lo < b.lo ? 1u : 0u;
  return U128{a.hi - b.hi - borrow, a.lo - b.lo};
}

U128 mul64(uint64_t a, uint64_t b) {
  const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
  const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
  const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
  return U128{p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xFFFFFFFFu)};
}

// a * b; false when the product needs more than 128 bits.
bool mul(const U128& a, const U128& b, U128& out) {
  if (a.hi != 0 && b.hi != 0) return false;
  const U128 low = mul64(a.lo, b.lo);
  uint64_t c1 = 0, c2 = 0, cross = 0, hi = 0;
  if (__builtin_mul_overflow(a.hi, b.lo, &c1)) return false;
  if (__builtin_mul_overflow(a.lo, b.hi, &c2)) return false;
  if (__builtin_add_overflow(c1, c2, &cross)) return false;
  if (__builtin_add_overflow(low.hi, cross, &hi)) return false;
  out = U128{hi, low.lo};
  return true;
}

unsigned ctz(const U128& a) {
  return a.lo != 0 ? (unsigned)__builtin_ctzll(a.lo) : 64u + (unsigned)__builtin_ctzll(a.hi);
}

unsigned bitLength(const U128& a) {
  if (a.hi != 0) return 128u - (unsigned)__builtin_clzll(a.hi);
  return a.lo != 0 ? 64u - (unsigned)__builtin_clzll(a.lo) : 0u;
}

U128 shr(const U128& a, unsigned k) {
  if (k == 0) return a;
  if (k >= 64) return U128{0, a.hi >> (k - 64)};
  return U128{a.hi >> k, (a.lo >> k) | (a.hi << (64 - k))};
}

U128 shl(const U128& a, unsigned k) {
  if (k == 0) return a;
  if (k >= 64) return U128{a.lo << (k - 64), 0};
  return U128{(a.hi << k) | (a.lo >> (64 - k)), a.lo << k};
}

bool bitAt(const U128& a, unsigned i) { return ((i >= 64 ? a.hi >> (i - 64) : a.lo >> i) & 1u) != 0; }

// q = a / b, r = a % b, for 0 < b < 2^127 (every divisor here is a value or a gcd of values
// below 2^124, so r * 2 never leaves 128 bits): shift and subtract, one bit at a time.
void divmod(const U128& a, const U128& b, U128& q, U128& r) {
  if (a.hi == 0 && b.hi == 0) {
    q = u128(a.lo / b.lo);
    r = u128(a.lo % b.lo);
    return;
  }
  q = U128{};
  r = U128{};
  for (unsigned i = bitLength(a); i-- > 0;) {
    r = shl(r, 1);
    r.lo |= bitAt(a, i) ? 1u : 0u;
    if (cmp(r, b) >= 0) {
      r = sub(r, b);
      q = i >= 64 ? U128{q.hi | (1ull << (i - 64)), q.lo} : U128{q.hi, q.lo | (1ull << i)};
    }
  }
}

U128 quot(const U128& a, const U128& b) {
  U128 q, r;
  divmod(a, b, q, r);
  return q;
}

// The first n bytes of s (all of s when shorter), and what follows byte k.
std::string_view head(std::string_view s, size_t n) { return std::string_view(s.data(), n < s.size() ? n : s.size()); }
std::string_view after(std::string_view s, size_t k) { return std::string_view(s.data() + k + 1, s.size() - k - 1); }

// a / d for a small divisor; returns the remainder.
uint32_t divSmall(const U128& a, uint32_t d, U128& q) {
  const uint32_t limbs[4] = {(uint32_t)(a.hi >> 32), (uint32_t)a.hi, (uint32_t)(a.lo >> 32), (uint32_t)a.lo};
  uint32_t out[4] = {};
  uint64_t rem = 0;
  for (int i = 0; i < 4; i++) {
    const uint64_t cur = (rem << 32) | limbs[i];
    out[i] = (uint32_t)(cur / d);
    rem = cur % d;
  }
  q = U128{((uint64_t)out[0] << 32) | out[1], ((uint64_t)out[2] << 32) | out[3]};
  return (uint32_t)rem;
}

// Binary gcd (a, b not both zero).
U128 gcd(U128 a, U128 b) {
  if (isZero(a)) return b;
  if (isZero(b)) return a;
  const unsigned za = ctz(a), zb = ctz(b);
  a = shr(a, za);
  while (!isZero(b)) {
    b = shr(b, ctz(b));
    if (cmp(a, b) > 0) {
      const U128 t = a;
      a = b;
      b = t;
    }
    b = sub(b, a);
  }
  return shl(a, za < zb ? za : zb);
}

// ---- exact fractions ---------------------------------------------------------------------
// Reduced, den >= 1, zero is 0/1 without a sign; num and den below 2^124 (kRangeBits).
constexpr unsigned kRangeBits = 124;

struct Q {
  U128 num, den = u128(1);
  bool neg = false;
};

bool inRange(const U128& a) { return bitLength(a) <= kRangeBits; }

Q negated(Q a) {
  if (!isZero(a.num)) a.neg = !a.neg;
  return a;
}

// num/den reduced into out; false when it leaves the range.
bool normalize(const U128& num, const U128& den, bool neg, Q& out) {
  if (isZero(num)) {
    out = Q{};
    return true;
  }
  const U128 g = gcd(num, den);
  out = Q{quot(num, g), quot(den, g), neg};
  return inRange(out.num) && inRange(out.den);
}

// a * b, cross-cancelled first so the product is reduced as it is formed.
bool mulQ(const Q& a, const Q& b, Q& out) {
  if (isZero(a.num) || isZero(b.num)) {
    out = Q{};
    return true;
  }
  const U128 g1 = gcd(a.num, b.den), g2 = gcd(b.num, a.den);
  Q r;
  if (!mul(quot(a.num, g1), quot(b.num, g2), r.num)) return false;
  if (!mul(quot(a.den, g2), quot(b.den, g1), r.den)) return false;
  r.neg = a.neg != b.neg;
  out = r;
  return inRange(r.num) && inRange(r.den);
}

// a + b over the least common denominator.
bool addQ(const Q& a, const Q& b, Q& out) {
  const U128 g = gcd(a.den, b.den);
  const U128 ka = quot(b.den, g), kb = quot(a.den, g);
  U128 den, n1, n2, sum;
  if (!mul(a.den, ka, den) || !mul(a.num, ka, n1) || !mul(b.num, kb, n2)) return false;
  bool neg = a.neg;
  if (a.neg == b.neg) {
    if (!add(n1, n2, sum)) return false;
  } else if (cmp(n1, n2) >= 0) {
    sum = sub(n1, n2);
  } else {
    sum = sub(n2, n1);
    neg = b.neg;
  }
  return normalize(sum, den, neg, out);
}

// ---- tokens ------------------------------------------------------------------------------
enum class T : uint8_t { End, Num, Plus, Minus, Star, Slash, LParen, RParen, Percent, Of, Eq };

struct Tok {
  T t = T::End;
  uint16_t at = 0;    // byte offset
  uint16_t len = 0;   // bytes
};

// What a value is, for the percent rules (calc.h).
enum class Kind : uint8_t {
  Plain,     // an ordinary number
  Bare,      // exactly a percent literal, N%
  Signed,    // a percent literal with signs, -N%
  Chain,     // N% of M%: still a percent, so another 'of' may follow
  Derived,   // a value computed from a percent without 'of'
};

// May stand before 'of'.
bool isPercent(Kind k) { return k == Kind::Bare || k == Kind::Signed || k == Kind::Chain; }

struct Val {
  Q q;
  Kind kind = Kind::Plain;
  uint16_t at = 0;   // byte offset where the operand starts
};

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isNumChar(char c) { return isDigit(c) || c == '.' || c == ',' ; }

// ---- numbers as people type them -------------------------------------------------------
struct Marks {
  char dec = 0, thou = 0;    // the decimal mark and the thousands mark, 0 when absent
};

size_t countOf(std::string_view s, char c) {
  size_t n = 0;
  for (char x : s) n += x == c ? 1u : 0u;
  return n;
}

// Which mark is which (calc.h: numbers). Error::None, BadNumber or UnclearNumber.
Error classify(std::string_view s, Marks& m) {
  const size_t dots = countOf(s, '.'), commas = countOf(s, ',');
  if (dots != 0 && commas != 0) {
    m.dec = s.rfind('.') > s.rfind(',') ? '.' : ',';
    m.thou = m.dec == '.' ? ',' : '.';
    return countOf(s, m.dec) == 1 ? Error::None : Error::BadNumber;
  }
  if (dots != 0) {
    (dots == 1 ? m.dec : m.thou) = '.';
    return Error::None;
  }
  if (commas == 0) return Error::None;
  if (commas > 1) {
    m.thou = ',';
    return Error::None;
  }
  const size_t before = s.find(','), after = s.size() - before - 1;
  if (before == 0 || after == 0) return Error::BadNumber;
  if (after == 3 && before <= 3 && s[0] != '0') return Error::UnclearNumber;
  m.dec = ',';
  return Error::None;
}

// The integer part's thousands groups: 1 to 3 digits not starting with 0, then groups of 3.
bool groupsValid(std::string_view ip, char thou) {
  if (thou == 0) return true;
  size_t start = 0;
  bool first = true;
  for (;;) {
    const size_t end = ip.find(thou, start);
    const size_t len = (end == std::string_view::npos ? ip.size() : end) - start;
    if (first ? (len == 0 || len > 3 || ip[start] == '0') : len != 3) return false;
    if (end == std::string_view::npos) return true;
    first = false;
    start = end + 1;
  }
}

// The digits of one part without marks into acc (acc = acc * 10 + digit).
void appendDigits(std::string_view part, U128& acc) {
  for (char c : part) {
    if (!isDigit(c)) continue;
    U128 t;
    mul(acc, u128(10), t);   // at most 36 digits: never leaves 128 bits
    add(t, u128((uint64_t)(c - '0')), acc);
  }
}

size_t significantInt(std::string_view ip) {
  size_t n = 0;
  bool lead = true;
  for (char c : ip) {
    if (!isDigit(c)) continue;
    if (lead && c == '0') continue;
    lead = false;
    n++;
  }
  return n;
}

Error parseNumber(std::string_view s, Q& out) {
  Marks m;
  const Error e = classify(s, m);
  if (e != Error::None) return e;
  const size_t p = m.dec ? s.find(m.dec) : s.size();
  const std::string_view ip(s.data(), p);
  std::string_view fp = p < s.size() ? std::string_view(s.data() + p + 1, s.size() - p - 1) : std::string_view();
  if (!groupsValid(ip, m.thou)) return Error::BadNumber;
  if (m.dec == ',' && fp.empty()) return Error::BadNumber;
  if (countOf(s, '.') + countOf(s, ',') == s.size()) return Error::BadNumber;   // no digit
  while (!fp.empty() && fp.back() == '0') fp.remove_suffix(1);
  if (significantInt(ip) > kMaxDigits || fp.size() > kMaxDigits) return Error::NumberTooLong;
  U128 num;
  appendDigits(ip, num);
  appendDigits(fp, num);
  U128 den = u128(1);
  for (size_t i = 0; i < fp.size(); i++) mul(den, u128(10), den);
  normalize(num, den, false, out);   // below 10^36: inside the range
  return Error::None;
}

// ---- the evaluator -----------------------------------------------------------------------
// Operator precedence without recursion: pending operators and their left operands wait on
// two fixed stacks. Every operator is left-associative, so a parenthesis level holds at most
// one pending operator per precedence (+ -, * /, of) and its '(' mark.
constexpr size_t kOpSlots = 4 * kMaxDepth + 3;
constexpr size_t kValSlots = 3 * (kMaxDepth + 1) + 1;

struct Pending {
  T t = T::End;        // Plus Minus Star Slash Of, or LParen for a '(' mark
  bool neg = false;    // LParen: the signs before it make the group negative
  uint16_t at = 0;     // byte offset of the operator or the '('
  uint16_t from = 0;   // LParen: where the operand starts (its first sign)
};

int precedence(T t) {
  switch (t) {
    case T::Plus:
    case T::Minus: return 1;
    case T::Star:
    case T::Slash: return 2;
    case T::Of: return 3;
    default: return 0;   // LParen: never reduced by an operator
  }
}

class Evaluator {
 public:
  explicit Evaluator(std::string_view s) : s_(s) {}

  bool run(Q& out);
  Error error() const { return err_; }
  size_t errAt() const { return errAt_; }
  size_t errLen() const { return errLen_; }

 private:
  bool fail(Error e, size_t at, size_t len = 0) {
    if (err_ == Error::None) {
      err_ = e;
      errAt_ = at;
      errLen_ = len;
    }
    return false;
  }
  bool advance();
  bool lexOther(size_t at);
  bool failExpectedOperator() {
    if (cur_.t == T::Percent) return fail(Error::PercentPlacement, cur_.at);
    return fail(Error::ExpectedOperator, cur_.at);
  }
  bool operand();
  bool literal(bool neg, bool anySign, uint16_t from);
  bool step();
  bool binary();
  bool close();
  bool finish(Q& out);
  bool reduceTo(int prec);
  bool apply(const Pending& op, Val& a, const Val& b);
  bool additive(const Pending& op, Val& a, const Val& b);

  std::string_view s_;
  size_t i_ = 0;
  Tok cur_;
  unsigned depth_ = 0;
  Pending ops_[kOpSlots];
  Val vals_[kValSlots];
  size_t nOps_ = 0, nVals_ = 0;
  Error err_ = Error::None;
  size_t errAt_ = 0, errLen_ = 0;
};

bool Evaluator::advance() {
  while (i_ < s_.size() && isSpace(s_[i_])) i_++;
  const size_t at = i_;
  if (at >= s_.size()) {
    cur_ = Tok{T::End, (uint16_t)at, 0};
    return true;
  }
  const char c = s_[at];
  if (isNumChar(c)) {
    while (i_ < s_.size() && isNumChar(s_[i_])) i_++;
    cur_ = Tok{T::Num, (uint16_t)at, (uint16_t)(i_ - at)};
    return true;
  }
  if (isLetter(c)) {
    while (i_ < s_.size() && isLetter(s_[i_])) i_++;
    const size_t len = i_ - at;
    if (len == 2 && (s_[at] | 0x20) == 'o' && (s_[at + 1] | 0x20) == 'f') {
      cur_ = Tok{T::Of, (uint16_t)at, 2};
      return true;
    }
    return fail(Error::UnknownWord, at, len);
  }
  return lexOther(at);
}

// Operators, parentheses and everything that is not a number or a word.
bool Evaluator::lexOther(size_t at) {
  static constexpr struct {
    char bytes[4];
    uint8_t len;
    T t;
  } kSigns[] = {
      {"**", 2, T::End},        {"+", 1, T::Plus},         {"-", 1, T::Minus},
      {"*", 1, T::Star},        {"/", 1, T::Slash},        {"(", 1, T::LParen},
      {")", 1, T::RParen},      {"%", 1, T::Percent},      {"=", 1, T::Eq},
      {"\xC3\x97", 2, T::Star}, {"\xC3\xB7", 2, T::Slash}, {"\xE2\x88\x92", 3, T::Minus},
  };
  const size_t left = s_.size() - at;
  for (const auto& sign : kSigns) {
    if (sign.len > left || std::memcmp(s_.data() + at, sign.bytes, sign.len) != 0) continue;
    if (sign.t == T::End) return fail(Error::Unsupported, at, sign.len);   // '**'
    i_ = at + sign.len;
    cur_ = Tok{sign.t, (uint16_t)at, sign.len};
    return true;
  }
  const unsigned char c = (unsigned char)s_[at];
  if (c == '^') return fail(Error::Unsupported, at, 1);
  return fail(Error::BadChar, at, (c > 0x20 && c < 0x7F) ? 1u : 0u);
}

// Expecting an operand: signs, then a number, or '(' and another operand (a loop, not a
// recursion: the level lives on the pending stack).
bool Evaluator::operand() {
  for (;;) {
    const uint16_t from = cur_.at;
    bool neg = false, anySign = false;
    while (cur_.t == T::Plus || cur_.t == T::Minus) {
      neg = neg != (cur_.t == T::Minus);
      anySign = true;
      if (!advance()) return false;
    }
    switch (cur_.t) {
      case T::Num: return literal(neg, anySign, from);
      case T::LParen:
        if (depth_ == kMaxDepth) return fail(Error::TooDeep, cur_.at);
        depth_++;
        ops_[nOps_++] = Pending{T::LParen, neg, cur_.at, from};
        if (!advance()) return false;
        continue;
      case T::Percent: return fail(Error::PercentPlacement, cur_.at);
      case T::Of: return fail(Error::OfPlacement, cur_.at);
      default: return fail(Error::ExpectedValue, cur_.at);
    }
  }
}

bool Evaluator::literal(bool neg, bool anySign, uint16_t from) {
  const Tok num = cur_;
  Val v;
  const Error e = parseNumber(std::string_view(s_.data() + num.at, num.len), v.q);
  if (e != Error::None) return fail(e, num.at, num.len);
  v.at = from;
  if (!advance()) return false;
  if (cur_.t == T::Percent) {
    const Q hundredth{u128(1), u128(100), false};
    mulQ(v.q, hundredth, v.q);   // a literal's denominator is at most 10^18: stays in range
    v.kind = anySign ? Kind::Signed : Kind::Bare;
    if (!advance()) return false;
  }
  if (neg) v.q = negated(v.q);
  vals_[nVals_++] = v;
  return true;
}

// Reduces pending operators of this level with precedence >= prec.
bool Evaluator::reduceTo(int prec) {
  while (nOps_ > 0 && ops_[nOps_ - 1].t != T::LParen && precedence(ops_[nOps_ - 1].t) >= prec) {
    const Pending op = ops_[--nOps_];
    const Val b = vals_[--nVals_];
    if (!apply(op, vals_[nVals_ - 1], b)) return false;
  }
  return true;
}

bool Evaluator::apply(const Pending& op, Val& a, const Val& b) {
  if (op.t == T::Plus || op.t == T::Minus) return additive(op, a, b);
  Q rhs = b.q;
  if (op.t == T::Slash) {
    if (isZero(b.q.num)) return fail(Error::DivByZero, op.at);
    rhs = Q{b.q.den, b.q.num, b.q.neg};
  }
  if (!mulQ(a.q, rhs, a.q)) return fail(Error::Overflow, op.at);
  if (op.t == T::Of) a.kind = b.kind == Kind::Plain ? Kind::Plain : Kind::Chain;
  else if (a.kind != Kind::Plain || b.kind != Kind::Plain) a.kind = Kind::Derived;
  return true;
}

// a + b or a - b; a bare N% on the right is N percent of a (calc.h: percent).
bool Evaluator::additive(const Pending& op, Val& a, const Val& b) {
  const bool minus = op.t == T::Minus;
  if (b.kind == Kind::Bare) {
    Q factor;
    const Q one{u128(1), u128(1), false};
    if (!addQ(one, minus ? negated(b.q) : b.q, factor) || !mulQ(a.q, factor, a.q))
      return fail(Error::Overflow, op.at);
    return true;
  }
  if (b.kind != Kind::Plain) return fail(Error::UnclearPercent, b.at);
  if (!addQ(a.q, minus ? negated(b.q) : b.q, a.q)) return fail(Error::Overflow, op.at);
  return true;
}

// An operator after an operand: reduce what binds as tightly, check the percent rules.
bool Evaluator::binary() {
  const Tok op = cur_;
  const int prec = precedence(op.t);
  if (!reduceTo(prec)) return false;
  const Val& left = vals_[nVals_ - 1];
  if (op.t == T::Of && !isPercent(left.kind)) return fail(Error::OfPlacement, op.at);
  if (prec == 1 && left.kind != Kind::Plain) return fail(Error::UnclearPercent, left.at);
  ops_[nOps_++] = Pending{op.t, false, op.at, 0};
  return advance() && operand();
}

// ')' after an operand: the level's value, signed, as an ordinary number.
bool Evaluator::close() {
  if (depth_ == 0) return fail(Error::ExtraClose, cur_.at);
  if (!reduceTo(1)) return false;
  const Pending open = ops_[--nOps_];
  depth_--;
  Val& v = vals_[nVals_ - 1];
  if (open.neg) v.q = negated(v.q);
  v.kind = Kind::Plain;
  v.at = open.from;
  return advance();
}

bool Evaluator::finish(Q& out) {
  if (!reduceTo(1)) return false;
  if (depth_ != 0) return fail(Error::MissingClose, ops_[nOps_ - 1].at);
  out = vals_[0].q;
  return true;
}

// One token after an operand: an operator, or ')'.
bool Evaluator::step() {
  switch (cur_.t) {
    case T::Plus:
    case T::Minus:
    case T::Star:
    case T::Slash:
    case T::Of: return binary();
    case T::RParen: return close();
    default: return failExpectedOperator();
  }
}

bool Evaluator::run(Q& out) {
  if (!advance()) return false;
  if (cur_.t == T::End) return fail(Error::Empty, 0);
  if (!operand()) return false;
  while (cur_.t != T::End && cur_.t != T::Eq)
    if (!step()) return false;
  if (cur_.t == T::Eq && !advance()) return false;   // one trailing '=' is ignored
  if (cur_.t != T::End) return failExpectedOperator();
  return finish(out);
}

// ---- the result line ---------------------------------------------------------------------
struct Line {
  char* buf;
  size_t len = 0;
  void put(std::string_view s) {
    for (char c : s)
      if (len + 1 < kLineMax) buf[len++] = c;
    buf[len] = 0;
  }
  void putUnsigned(size_t v) {
    char d[24];
    size_t n = 0;
    do {
      d[n++] = (char)('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (n > 0) put(std::string_view(&d[--n], 1));
  }
};

// Decimal digits of a (at most 39), most significant first; returns the count.
size_t decimalDigits(U128 a, char* out) {
  char rev[40];
  size_t n = 0;
  do {
    rev[n++] = (char)('0' + divSmall(a, 10, a));
  } while (!isZero(a));
  for (size_t i = 0; i < n; i++) out[i] = rev[n - 1 - i];
  return n;
}

// The places an exact value needs, or kExactPlaces + 1 when it needs more or never ends.
unsigned placesNeeded(const U128& den) {
  const unsigned twos = ctz(den);
  U128 rest = shr(den, twos), q;
  unsigned fives = 0;
  while (fives <= kExactPlaces && divSmall(rest, 5, q) == 0) {
    rest = q;
    fives++;
  }
  const unsigned places = twos > fives ? twos : fives;
  return (cmp(rest, u128(1)) == 0 && places <= kExactPlaces) ? places : kExactPlaces + 1;
}

// Adds one unit in the last place of a digit string; returns the new length (a carry out of
// the top digit prepends a 1).
size_t roundUp(char* digits, size_t n) {
  for (size_t i = n; i-- > 0;) {
    if (digits[i] != '9') {
      digits[i]++;
      return n;
    }
    digits[i] = '0';
  }
  for (size_t i = n; i > 0; i--) digits[i] = digits[i - 1];
  digits[0] = '1';
  return n + 1;
}

void renderValue(const Q& v, Line& line, bool& rounded) {
  U128 ip, r;
  divmod(v.num, v.den, ip, r);
  char digits[64];
  size_t nInt = decimalDigits(ip, digits);
  const unsigned need = placesNeeded(v.den);
  rounded = need > kExactPlaces;
  const unsigned places = rounded ? kRoundedPlaces + 1 : need;
  for (unsigned k = 0; k < places; k++) {
    U128 t;
    mul(r, u128(10), t);   // r < den < 2^124
    char d = '0';
    while (cmp(t, v.den) >= 0) {
      t = sub(t, v.den);
      d++;
    }
    digits[nInt + k] = d;
    r = t;
  }
  size_t total = nInt + places;
  if (rounded) {
    const bool up = digits[--total] >= '5';
    if (up && roundUp(digits, total) > total) {
      total++;
      nInt++;
    }
  }
  while (total > nInt && digits[total - 1] == '0') total--;
  const bool allZero = total == 1 && digits[0] == '0';
  line.put("= ");
  if (v.neg && !allZero) line.put("-");
  line.put(std::string_view(digits, nInt));
  if (total > nInt) {
    line.put(".");
    line.put(std::string_view(digits + nInt, total - nInt));
  }
  if (rounded) line.put(" (rounded to 6 decimal places)");
}

size_t charPosition(std::string_view s, size_t byteOff) {
  size_t n = 0;
  for (size_t i = 0; i < byteOff && i < s.size(); i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n + 1;
}

// The reasons that carry no text from the input.
void putFixedReason(Error e, Line& line) {
  static_assert(kMaxExpr == 256 && kMaxDepth == 10 && kMaxDigits == 18 && kRoundedPlaces == 6,
                "the reason texts state these limits");
  static constexpr struct {
    Error e;
    const char* text;
  } kReasons[] = {
      {Error::ExpectedValue, "expected a number or '('"},
      {Error::ExpectedOperator, "expected an operator or ')'"},
      {Error::MissingClose, "missing ')'"},
      {Error::ExtraClose, "')' without '('"},
      {Error::TooDeep, "more than 10 nested parentheses"},
      {Error::NumberTooLong, "more than 18 digits before or after the point"},
      {Error::BadNumber, "number not understood (digits, one decimal mark, thousands in groups of 3)"},
      {Error::PercentPlacement, "'%' must follow a number"},
      {Error::OfPlacement, "'of' must follow a percent, as in 15% of 80"},
      {Error::UnclearPercent, "unclear percent (N% of X, or a percent in parentheses, is clear)"},
      {Error::DivByZero, "division by zero"},
      {Error::Overflow, "too large to compute exactly"},
  };
  for (const auto& r : kReasons)
    if (r.e == e) line.put(r.text);
}

// The reason after "error at position N: " (calc.h lists them).
void putReason(Error e, std::string_view span, Line& line) {
  switch (e) {
    case Error::Empty: line.put("empty expression"); break;
    case Error::TooLong: line.put("longer than 256 bytes"); break;
    case Error::BadChar:
      if (span.empty()) {
        line.put("unexpected character");
      } else {
        line.put("unexpected '");
        line.put(span);
        line.put("'");
      }
      break;
    case Error::UnknownWord:
      line.put("unknown word '");
      line.put(head(span, 12));
      line.put("' (calc reads numbers, + - * / ( ) % and of)");
      break;
    case Error::Unsupported:
      line.put("'");
      line.put(span);
      line.put("' is not supported (calc reads + - * / ( ) %)");
      break;
    case Error::UnclearNumber: {
      const size_t comma = span.find(',');
      line.put("'");
      line.put(span);
      line.put("' could be ");
      line.put(head(span, comma));
      line.put(after(span, comma));
      line.put(" or ");
      line.put(head(span, comma));
      line.put(".");
      line.put(after(span, comma));
      line.put("");
      break;
    }
    default: putFixedReason(e, line); break;
  }
}

}  // namespace

Result evaluate(std::string_view expression) {
  Result r;
  Line line{r.line};
  std::string_view span;
  if (expression.size() > kMaxExpr) {
    r.error = Error::TooLong;
    r.position = (uint16_t)charPosition(expression, kMaxExpr);
  } else {
    Evaluator p(expression);
    Q v;
    if (p.run(v)) {
      renderValue(v, line, r.rounded);
    } else {
      r.error = p.error();
      r.position = (uint16_t)charPosition(expression, p.errAt());
      span = std::string_view(expression.data() + p.errAt(), p.errLen());
    }
  }
  if (!r.ok()) {
    line.put("error at position ");
    line.putUnsigned(r.position);
    line.put(": ");
    putReason(r.error, span, line);
  }
  r.len = (uint8_t)line.len;
  return r;
}

}  // namespace nimbus::orch::calc
