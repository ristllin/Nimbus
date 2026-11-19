// calc: exact decimal arithmetic on one expression (calc.h, lane RB-REC8).
//
// Class rules proven here:
//   - every accepted form gives the exact figure, and every refused form gives the exact
//     error line with its position (two tables, and every Error value is reached by one);
//   - against an independent big-integer reference (reference.h) over 100,000 generated
//     expressions (precedence, parentheses, unary signs, every percent reading, every number
//     format, typed operators, spaces, division by zero, numbers near the range limit, input
//     over the length limit): the line is the reference's figure, or the reference's first
//     division by zero, or an Overflow exactly where the reference says a value leaves the
//     range; never another number;
//   - arbitrary bytes never produce anything but a result line or one error line, inside
//     kLineMax, printable ASCII, with a position inside the input;
//   - no heap: a counting allocator sees zero allocations, and a refusing one changes no byte;
//   - nesting is bounded: kMaxDepth parentheses evaluate, one more is TooDeep.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "nimbus/orch/calc.h"
#include "reference.h"

using nimbus::orch::calc::Error;
using nimbus::orch::calc::evaluate;
namespace calc = nimbus::orch::calc;

// ---- a counting and refusing allocator ----------------------------------------------------
static bool g_counting = false;
static bool g_refuse = false;
static size_t g_allocs = 0;

void* operator new(size_t n) {
  if (g_counting) {
    g_allocs++;
    if (g_refuse) throw std::bad_alloc();
  }
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

void setUp() {}
void tearDown() {}

namespace {

std::string run(const std::string& s) { return std::string(evaluate(s).text()); }

struct Case {
  const char* in;
  const char* out;
};

// ---- accepted forms ----------------------------------------------------------------------
const Case kAccepted[] = {
    {"0.1+0.2", "= 0.3"},
    {"2300-1612.5", "= 687.5"},
    {"250*3.12/100", "= 7.8"},
    {"100/3*3", "= 100"},
    {"1/3", "= 0.333333 (rounded to 6 decimal places)"},
    {"2/3", "= 0.666667 (rounded to 6 decimal places)"},
    {"-2/3", "= -0.666667 (rounded to 6 decimal places)"},
    {"2+3*4", "= 14"},
    {"(2+3)*4", "= 20"},
    {"10-4-3", "= 3"},
    {"100/10/5", "= 2"},
    {"2*-3", "= -6"},
    {"--5", "= 5"},
    {"-(2+3)", "= -5"},
    {"3 - -2", "= 5"},
    {"+5", "= 5"},
    {"-0", "= 0"},
    {"1.5-1.5", "= 0"},
    {"15% of 80", "= 12"},
    {"15 % of 80", "= 12"},
    {"15%of 80", "= 12"},
    {"15% OF 80", "= 12"},
    {"80+15%", "= 92"},
    {"80-15%", "= 68"},
    {"100+10+5%", "= 115.5"},
    {"15%", "= 0.15"},
    {"-15%", "= -0.15"},
    {"200*15%", "= 30"},
    {"15%*200", "= 30"},
    {"(15%)", "= 0.15"},
    {"80+(15%)", "= 80.15"},
    {"15% of 80 + 5", "= 17"},
    {"5 + 15% of 80", "= 17"},
    {"50% of 20% of 80", "= 8"},
    {"-15% of 80", "= -12"},
    {"15% of -80", "= -12"},
    {"12.5% of 80", "= 10"},
    {"2300 - 15% of 2300", "= 1955"},
    {"(80+15%)*2", "= 184"},
    {"1,234.56+0.44", "= 1235"},
    {"1.234,56", "= 1234.56"},
    {"1,234,567", "= 1234567"},
    {"1.234.567", "= 1234567"},
    {"1.234", "= 1.234"},
    {"1,5+1,25", "= 2.75"},
    {"0,250", "= 0.25"},
    {"1234,5", "= 1234.5"},
    {"1,2345", "= 1.2345"},
    {"1,234.", "= 1234"},
    {"2 \xC3\x97 3", "= 6"},
    {"7 \xC3\xB7 2", "= 3.5"},
    {"5 \xE2\x88\x92 2", "= 3"},
    {"2+2=", "= 4"},
    {" 2 + 2 = ", "= 4"},
    {"\t2\n+\r\n2", "= 4"},
    {"007 + 1", "= 8"},
    {"5.", "= 5"},
    {".5", "= 0.5"},
    {"0.000000125", "= 0.000000125"},
    {"1/1024", "= 0.0009765625"},
    {"1/4096", "= 0.000244140625"},
    {"1/8192", "= 0.000122 (rounded to 6 decimal places)"},
    {"1/30000000", "= 0 (rounded to 6 decimal places)"},
    {"-1/30000000", "= 0 (rounded to 6 decimal places)"},
    {"0.0000005 + 0.0000000000001", "= 0.000001 (rounded to 6 decimal places)"},
    {"0.9999996", "= 0.9999996"},
    {"0.99999999999999", "= 1 (rounded to 6 decimal places)"},
    {"-9.99999999999999", "= -10 (rounded to 6 decimal places)"},
    {"0.1000000000000000000000", "= 0.1"},
    {"999999999999999999*999999999999999999", "= 999999999999999998000000000000000001"},
    {"999999999999999999.999999999999999999", "= 1000000000000000000 (rounded to 6 decimal places)"},
    {"((((((((((1))))))))))", "= 1"},
    {"2*(3+(4-(1/(2+(3*(1+1))))))", "= 13.75"},
};

// ---- refused forms -----------------------------------------------------------------------
const Case kRefused[] = {
    {"", "error at position 1: empty expression"},
    {"  \t ", "error at position 1: empty expression"},
    {"1/0", "error at position 2: division by zero"},
    {"1/(2-2)", "error at position 2: division by zero"},
    {"5/0%", "error at position 2: division by zero"},
    {"1/0.000", "error at position 2: division by zero"},
    {"2 \xC3\x97 3 / 0", "error at position 7: division by zero"},
    {"2 3", "error at position 3: expected an operator or ')'"},
    {"2(3)", "error at position 2: expected an operator or ')'"},
    {"(2 3)", "error at position 4: expected an operator or ')'"},
    {"2+2=3", "error at position 5: expected an operator or ')'"},
    {"1 000", "error at position 3: expected an operator or ')'"},
    {"(2+3", "error at position 1: missing ')'"},
    {"((2+3)", "error at position 1: missing ')'"},
    {"2+3)", "error at position 4: ')' without '('"},
    {"()", "error at position 2: expected a number or '('"},
    {"2+", "error at position 3: expected a number or '('"},
    {"*2", "error at position 1: expected a number or '('"},
    {"2*/3", "error at position 3: expected a number or '('"},
    {"=", "error at position 1: expected a number or '('"},
    {"2^3", "error at position 2: '^' is not supported: use + - * / ( ) %"},
    {"2**3", "error at position 2: '**' is not supported: use + - * / ( ) %"},
    {"sqrt(4)", "error at position 1: unknown word 'sqrt': use numbers, + - * / ( ) % and of"},
    {"2 x 3", "error at position 3: unknown word 'x': use numbers, + - * / ( ) % and of"},
    {"1e5", "error at position 2: unknown word 'e': use numbers, + - * / ( ) % and of"},
    {"abcdefghijklmnop", "error at position 1: unknown word 'abcdefghijkl': use numbers, + - * / ( ) % and of"},
    {"2 $ 3", "error at position 3: unexpected '$'"},
    {"$12.50", "error at position 1: unexpected '$'"},
    {"2 \xE2\x82\xAC 3", "error at position 3: unexpected character"},
    {"2 \x01 3", "error at position 3: unexpected character"},
    {"2 \xFF 3", "error at position 3: unexpected character"},
    {"1'000", "error at position 2: unexpected '''"},
    {"1_000", "error at position 2: unexpected '_'"},
    {"1,234", "error at position 1: '1,234' could be 1234 or 1.234: write one of them"},
    {"5 + 2,500", "error at position 5: '2,500' could be 2500 or 2.500: write one of them"},
    {"123,456", "error at position 1: '123,456' could be 123456 or 123.456: write one of them"},
    {"1,23,4", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"12,34,567", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"1.2.3", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"1,234.5,6", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"1.234,5.6", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"0,123.5", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"1.234,", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {".", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"1,", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {",5", "error at position 1: number not understood: use digits with one '.' for decimals"},
    {"2+,", "error at position 3: number not understood: use digits with one '.' for decimals"},
    {"1234567890123456789", "error at position 1: more than 18 digits before or after the point"},
    {"0.1234567890123456789", "error at position 1: more than 18 digits before or after the point"},
    {"%5", "error at position 1: '%' must follow a number"},
    {"(5)%", "error at position 4: '%' must follow a number"},
    {"5%%", "error at position 3: '%' must follow a number"},
    {"80 of 5", "error at position 4: 'of' must follow a percent, as in 15% of 80"},
    {"of 5", "error at position 1: 'of' must follow a percent, as in 15% of 80"},
    {"(15%) of 80", "error at position 7: 'of' must follow a percent, as in 15% of 80"},
    {"15% of 20 of 80", "error at position 11: 'of' must follow a percent, as in 15% of 80"},
    {"15%+5", "error at position 1: unclear percent: write N% of X, or put it in parentheses"},
    {"50%+50%", "error at position 1: unclear percent: write N% of X, or put it in parentheses"},
    {"80+2*15%", "error at position 4: unclear percent: write N% of X, or put it in parentheses"},
    {"80+-15%", "error at position 4: unclear percent: write N% of X, or put it in parentheses"},
    {"80+50% of 20%", "error at position 4: unclear percent: write N% of X, or put it in parentheses"},
    {"200*15%+5", "error at position 1: unclear percent: write N% of X, or put it in parentheses"},
    {"((((((((((((1))))))))))))", "error at position 11: more than 10 nested parentheses"},
    {"999999999999999999*999999999999999999*100",
     "error at position 38: too large to compute exactly"},
    {"1/999999999999999999/999999999999999999/100",
     "error at position 40: too large to compute exactly"},
    {"0.000000000000000001*0.000000000000000001/100000",
     "error at position 42: too large to compute exactly"},
    // The two cross numerators fit in 128 bits, their sum does not (a wrapped sum would
    // reduce to an in-range wrong figure).
    {"817584165233875340*318/740460644283772927+861172268078593469*488/183014892035689449",
     "error at position 42: too large to compute exactly"},
};

void checkTable(const Case* cases, size_t n) {
  for (size_t i = 0; i < n; i++) {
    const std::string got = run(cases[i].in);
    if (got != cases[i].out) {
      char msg[512];
      std::snprintf(msg, sizeof msg, "input [%s]: got [%s] want [%s]", cases[i].in, got.c_str(),
                    cases[i].out);
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

void test_accepted_forms_give_the_exact_figure() {
  checkTable(kAccepted, sizeof(kAccepted) / sizeof(kAccepted[0]));
}

void test_refused_forms_give_the_exact_error_line() {
  checkTable(kRefused, sizeof(kRefused) / sizeof(kRefused[0]));
}

void test_result_fields_match_the_line() {
  const calc::Result ok = evaluate("1/3");
  TEST_ASSERT_TRUE(ok.ok());
  TEST_ASSERT_TRUE(ok.rounded);
  TEST_ASSERT_EQUAL(0, ok.position);
  TEST_ASSERT_EQUAL(std::strlen(ok.line), ok.len);
  const calc::Result exact = evaluate("0.1+0.2");
  TEST_ASSERT_FALSE(exact.rounded);
  const calc::Result bad = evaluate("2 \xC3\x97 3 / 0");
  TEST_ASSERT_EQUAL((int)Error::DivByZero, (int)bad.error);
  TEST_ASSERT_EQUAL(7, bad.position);
  TEST_ASSERT_FALSE(bad.rounded);
}

// Every Error value is the answer to at least one refused case (a new value with no case fails).
void test_every_error_value_is_reached() {
  bool seen[(int)Error::Overflow + 1] = {};
  for (const Case& c : kRefused) seen[(int)evaluate(c.in).error] = true;
  std::string tooLong(calc::kMaxExpr + 1, '1');
  seen[(int)evaluate(tooLong).error] = true;
  for (int e = 1; e <= (int)Error::Overflow; e++) {
    char msg[64];
    std::snprintf(msg, sizeof msg, "Error value %d has no case", e);
    TEST_ASSERT_TRUE_MESSAGE(seen[e], msg);
  }
}

void test_length_limit_is_exact() {
  std::string s = "1";
  while (s.size() + 2 <= calc::kMaxExpr) s += "+1";
  if (s.size() < calc::kMaxExpr) s += " ";
  TEST_ASSERT_EQUAL(calc::kMaxExpr, s.size());
  TEST_ASSERT_EQUAL_STRING("= 128", run(s).c_str());
  s += "1";
  TEST_ASSERT_EQUAL_STRING("error at position 257: longer than 256 bytes", run(s).c_str());
  // Positions count characters: 128 two-byte signs and one more byte.
  std::string wide;
  while (wide.size() < calc::kMaxExpr) wide += "\xC3\x97";
  wide += "1";
  TEST_ASSERT_EQUAL_STRING("error at position 129: longer than 256 bytes", run(wide).c_str());
}

void test_nesting_is_bounded() {
  for (unsigned depth = 1; depth <= calc::kMaxDepth + 1; depth++) {
    const std::string s = std::string(depth, '(') + "2" + std::string(depth, ')');
    const calc::Result r = evaluate(s);
    if (depth <= calc::kMaxDepth) {
      TEST_ASSERT_EQUAL_STRING("= 2", std::string(r.text()).c_str());
    } else {
      TEST_ASSERT_EQUAL((int)Error::TooDeep, (int)r.error);
      TEST_ASSERT_EQUAL(depth, r.position);
    }
  }
  // Signs and 'of' chains do not nest: a long run of them is fine.
  std::string signs(200, '-');
  TEST_ASSERT_EQUAL_STRING("= 3", run(signs + "3").c_str());
  std::string chain;
  for (int i = 0; i < 30; i++) chain += "100% of ";
  TEST_ASSERT_EQUAL_STRING("= 7", run(chain + "7").c_str());
}

// ---- the generator and the reference -----------------------------------------------------
enum class K { Lit, Neg, Paren, Bin, OfChain, AddPct };

struct Node {
  K k = K::Lit;
  std::string I, F;          // Lit: integer digits (no leading zero unless "0"), fraction digits
  bool pct = false;          // Lit: a percent literal
  std::string signs;         // Neg: the signs as typed ("-", "+-", "--")
  char op = 0;               // Bin and AddPct: + - * /
  std::vector<int> kids;     // Bin: l r; Neg, Paren: child; AddPct: l pctLit; OfChain: pcts... x
  std::vector<size_t> pos;   // byte offsets of this node's operator tokens
};

enum class Ev { Add, Sub, Mul, Div };

struct Event {
  Ev kind;
  size_t pos;
  ref::Rat a, b;
};

struct Gen {
  std::mt19937_64 rng;
  std::vector<Node> pool;
  std::string out;
  bool big = false;

  explicit Gen(uint64_t seed) : rng(seed) {}
  uint64_t r(uint64_t n) { return rng() % n; }
  bool p(unsigned pct) { return r(100) < pct; }

  int add(Node n) {
    pool.push_back(std::move(n));
    return (int)pool.size() - 1;
  }

  std::string digits(size_t n, bool leadNonZero) {
    std::string s;
    for (size_t i = 0; i < n; i++) s.push_back((char)('0' + ((i == 0 && leadNonZero) ? 1 + r(9) : r(10))));
    return s;
  }

  int lit(bool pct) {
    Node n;
    n.pct = pct;
    if (p(6)) {
      n.I = "0";
      n.F = p(50) ? "" : std::string(1 + r(3), '0');
      return add(n);
    }
    const size_t maxI = big ? 18 : 7, maxF = big ? 18 : 4;
    const size_t li = r(maxI + 1);
    n.I = li == 0 ? "0" : digits(li, true);
    n.F = p(45) ? "" : digits(1 + r(maxF), false);
    return add(n);
  }

  int gen(int depth) {
    const uint64_t c = depth <= 0 ? 0 : r(100);
    if (c < 30) return lit(false);
    Node n;
    if (c < 40) {
      n.k = K::Neg;
      const size_t count = 1 + r(3);
      for (size_t i = 0; i < count; i++) n.signs.push_back(p(75) ? '-' : '+');
      if (n.signs.find('-') == std::string::npos) n.signs[0] = '-';
      n.kids = {gen(depth - 1)};
    } else if (c < 47) {
      n.k = K::Paren;
      n.kids = {gen(depth - 1)};
    } else if (c < 85) {
      n.k = K::Bin;
      n.op = "+-*/"[r(4)];
      if (n.op == '/' && p(8)) n.kids = {gen(depth - 1), zeroish(depth - 1)};
      else n.kids = {gen(depth - 1), gen(depth - 1)};
    } else if (c < 93) {
      n.k = K::OfChain;
      const size_t count = 1 + (p(25) ? r(3) : 0);
      for (size_t i = 0; i < count; i++) n.kids.push_back(signedPct());
      n.kids.push_back(gen(depth - 1));
    } else {
      n.k = K::AddPct;
      n.op = p(50) ? '+' : '-';
      n.kids = {gen(depth - 1), lit(true)};
    }
    return add(n);
  }

  // A percent literal, sometimes with signs (allowed before 'of').
  int signedPct() {
    const int l = lit(true);
    if (!p(15)) return l;
    Node n;
    n.k = K::Neg;
    n.signs = p(50) ? "-" : "+-";
    n.kids = {l};
    return add(n);
  }

  // A divisor that is zero: a zero literal or x - x.
  int zeroish(int depth) {
    if (p(50) || depth <= 0) {
      Node n;
      n.I = "0";
      n.F = p(50) ? "" : "0";
      return add(n);
    }
    const int x = gen(depth - 1);
    const int y = clone(x);
    Node n;
    n.k = K::Bin;
    n.op = '-';
    n.kids = {x, y};   // two copies of one subtree: equal values
    return add(n);
  }

  int clone(int id) {
    Node n = pool[id];
    for (int& k : n.kids) k = clone(k);
    return add(n);
  }

  // The root: an expression, or with a small chance a term that holds a percent factor.
  int root(int depth) {
    if (!p(6)) return gen(depth);
    const int pc = lit(true);
    if (p(20)) return pc;
    Node n;
    n.k = K::Bin;
    const uint64_t c = r(3);
    n.op = c == 0 ? '/' : '*';
    n.kids = c == 1 ? std::vector<int>{pc, gen(depth - 1)} : std::vector<int>{gen(depth - 1), pc};
    return add(n);
  }

  // ---- rendering -------------------------------------------------------------------------
  void space() {
    if (p(35)) out += p(85) ? " " : (p(50) ? "\t" : "  ");
  }

  static int prec(const Node& n) {
    switch (n.k) {
      case K::Lit: return 5;
      case K::Paren: return 5;
      case K::Neg: return 4;
      case K::OfChain: return 3;
      case K::Bin: return (n.op == '+' || n.op == '-') ? 1 : 2;
      case K::AddPct: return 1;
    }
    return 0;
  }

  static std::string group(const std::string& I, char mark) {
    std::string s;
    const size_t first = I.size() % 3 == 0 ? 3 : I.size() % 3;
    s = I.substr(0, first);
    for (size_t i = first; i < I.size(); i += 3) s += std::string(1, mark) + I.substr(i, 3);
    return s;
  }

  // One of the number formats people type, always an unambiguous one.
  static std::vector<std::string> groupedForms(const std::string& I, const std::string& F) {
    if (I.size() < 7 && (I.size() < 4 || F.empty())) return {};
    if (F.empty()) return {group(I, ','), group(I, '.')};
    return {group(I, ',') + "." + F, group(I, '.') + "," + F};
  }

  std::string number(const Node& n) {
    const std::string& I = n.I;
    const std::string& F = n.F;
    const std::string dotF = F.empty() ? "" : "." + F;
    std::vector<std::string> forms = groupedForms(I, F);
    forms.insert(forms.begin(), I + dotF);
    const bool commaUnclear = F.size() == 3 && I.size() <= 3 && I[0] != '0';
    if (!F.empty() && !commaUnclear) forms.push_back(I + "," + F);
    if (I == "0" && !F.empty()) forms.push_back("." + F);
    if (F.empty()) forms.push_back(I + ".");
    if (I.size() < 18) forms.push_back("0" + I + dotF);
    return p(40) ? forms[0] : forms[r(forms.size())];
  }

  void opChar(char op) {
    if (op == '*' && p(10)) out += "\xC3\x97";
    else if (op == '/' && p(10)) out += "\xC3\xB7";
    else if (op == '-' && p(10)) out += "\xE2\x88\x92";
    else out.push_back(op);
  }

  void child(int id, bool parens) {
    if (parens) {
      out += "(";
      space();
    }
    render(id);
    if (parens) {
      space();
      out += ")";
    }
  }

  void render(int id) {
    Node& n = pool[id];
    n.pos.clear();
    switch (n.k) {
      case K::Lit:
        out += number(n);
        if (n.pct) out += p(10) ? " %" : "%";
        break;
      case K::Neg:
        for (char s : n.signs) {
          opChar(s);
          if (p(10)) out += " ";
        }
        child(n.kids[0], prec(pool[n.kids[0]]) < 4);
        break;
      case K::Paren:
        child(n.kids[0], true);
        break;
      case K::Bin: {
        const int pr = prec(n);
        child(n.kids[0], prec(pool[n.kids[0]]) < pr);
        space();
        n.pos.push_back(out.size());
        opChar(n.op);
        space();
        child(n.kids[1], prec(pool[n.kids[1]]) <= pr);
        break;
      }
      case K::OfChain:
        for (size_t i = 0; i + 1 < n.kids.size(); i++) {
          render(n.kids[i]);
          out += p(10) ? "" : " ";
          n.pos.push_back(out.size());
          out += p(5) ? "OF" : "of";
          out += " ";
        }
        child(n.kids.back(), prec(pool[n.kids.back()]) < 4);
        break;
      case K::AddPct:
        child(n.kids[0], false);
        space();
        n.pos.push_back(out.size());
        opChar(n.op);
        space();
        render(n.kids[1]);
        break;
    }
  }
};

ref::Rat litValue(const Node& n) {
  ref::Rat v;
  v.num = ref::fromDigits(n.I + n.F);
  v.den = ref::pow10((unsigned)n.F.size() + (n.pct ? 2u : 0u));
  return v;
}

// Evaluates in the order the calculator does, recording every operation as an event.
struct Eval {
  const std::vector<Node>& pool;
  std::vector<Event> events;

  ref::Rat op(Ev kind, size_t pos, const ref::Rat& a, const ref::Rat& b) {
    events.push_back({kind, pos, a, b});
    ref::Rat r;
    switch (kind) {
      case Ev::Add: r = ref::addR(a, b); break;
      case Ev::Sub: r = ref::addR(a, ref::neg(b)); break;
      case Ev::Mul: r = ref::mulR(a, b); break;
      case Ev::Div: r = b.num.zero() ? ref::Rat{} : ref::mulR(a, ref::invR(b)); break;
    }
    if (r.num.bits() + r.den.bits() > 600) r = ref::reduce(r);
    return r;
  }

  static Ev evOf(char c) {
    return c == '+' ? Ev::Add : c == '-' ? Ev::Sub : c == '*' ? Ev::Mul : Ev::Div;
  }

  ref::Rat ofChain(const Node& n) {
    ref::Rat a = eval(n.kids[0]);
    for (size_t i = 1; i + 1 < n.kids.size(); i++) a = op(Ev::Mul, n.pos[i - 1], a, eval(n.kids[i]));
    const ref::Rat x = eval(n.kids.back());
    return op(Ev::Mul, n.pos.back(), a, x);
  }

  ref::Rat eval(int id) {
    const Node& n = pool[id];
    switch (n.k) {
      case K::Lit: return litValue(n);
      case K::Neg: {
        ref::Rat v = eval(n.kids[0]);
        for (char s : n.signs)
          if (s == '-') v = ref::neg(v);
        return v;
      }
      case K::Paren: return eval(n.kids[0]);
      case K::Bin: {
        const ref::Rat a = eval(n.kids[0]);
        const ref::Rat b = eval(n.kids[1]);
        return op(evOf(n.op), n.pos[0], a, b);
      }
      case K::OfChain: return ofChain(n);
      case K::AddPct: {
        const ref::Rat a = eval(n.kids[0]);
        ref::Rat f = litValue(pool[n.kids[1]]);
        f = ref::addR(ref::Rat{false, ref::fromU64(1), ref::fromU64(1)}, n.op == '+' ? f : ref::neg(f));
        return op(Ev::Mul, n.pos[0], a, f);
      }
    }
    return ref::Rat{};
  }
};

constexpr size_t kRangeBits = 124;    // values: numerator and denominator below 2^124
constexpr size_t kWorkBits = 128;     // the calculator's working integers

ref::Big quot(const ref::Big& a, const ref::Big& b) {
  ref::Big q, r;
  ref::divmod(a, b, q, r);
  return q;
}

// Whether the calculator's exact method leaves its integers for this operation. Mirrors the
// documented method (reduced operands; product: cross-cancelled; sum: over the least common
// denominator), computed with unbounded integers.
bool leavesRange(const Event& e) {
  ref::Rat a = ref::reduce(e.a), b = ref::reduce(e.b);
  if (e.kind == Ev::Div) b = ref::invR(b);
  if (e.kind == Ev::Mul || e.kind == Ev::Div) {
    const ref::Big g1 = ref::gcd(a.num, b.den), g2 = ref::gcd(b.num, a.den);
    if (a.num.zero() || b.num.zero()) return false;
    const ref::Big n = ref::mul(quot(a.num, g1), quot(b.num, g2));
    const ref::Big d = ref::mul(quot(a.den, g2), quot(b.den, g1));
    return n.bits() > kRangeBits || d.bits() > kRangeBits;
  }
  if (e.kind == Ev::Sub) b = ref::neg(b);
  const ref::Big g = ref::gcd(a.den, b.den);
  const ref::Big den = ref::mul(quot(a.den, g), b.den);
  const ref::Big n1 = ref::mul(a.num, quot(b.den, g)), n2 = ref::mul(b.num, quot(a.den, g));
  if (den.bits() > kWorkBits || n1.bits() > kWorkBits || n2.bits() > kWorkBits) return true;
  const ref::Rat s = ref::reduce(ref::addR(a, b));
  if (ref::add(n1, n2).bits() > kWorkBits && a.neg == b.neg) return true;
  return s.num.bits() > kRangeBits || s.den.bits() > kRangeBits;
}

// Cheap bound: unreduced operands small enough that no method can leave the range.
bool surelyInRange(const Event& e) {
  const size_t an = e.a.num.bits(), ad = e.a.den.bits(), bn = e.b.num.bits(), bd = e.b.den.bits();
  if (e.kind == Ev::Mul) return an + bn <= kRangeBits && ad + bd <= kRangeBits;
  if (e.kind == Ev::Div) return an + bd <= kRangeBits && ad + bn <= kRangeBits;
  return an + bd + 1 <= kRangeBits && bn + ad + 1 <= kRangeBits && ad + bd <= kRangeBits;
}

size_t charPos(const std::string& s, size_t byteOff) {
  size_t n = 0;
  for (size_t i = 0; i < byteOff && i < s.size(); i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n + 1;
}

struct Stats {
  size_t ok = 0, rounded = 0, divZero = 0, overflow = 0, tooLong = 0, pct = 0, unicode = 0;
};

std::string expected(Gen& g, int rootId, Stats& st) {
  if (g.out.size() > calc::kMaxExpr) {
    st.tooLong++;
    return "error at position " + std::to_string(charPos(g.out, calc::kMaxExpr)) +
           ": longer than 256 bytes";
  }
  Eval ev{g.pool, {}};
  const ref::Rat v = ev.eval(rootId);
  for (const Event& e : ev.events) {
    if (e.kind == Ev::Div && e.b.num.zero()) {
      st.divZero++;
      return "error at position " + std::to_string(charPos(g.out, e.pos)) + ": division by zero";
    }
    if (!surelyInRange(e) && leavesRange(e)) {
      st.overflow++;
      return "error at position " + std::to_string(charPos(g.out, e.pos)) +
             ": too large to compute exactly";
    }
  }
  const std::string line = ref::render(v);
  st.ok++;
  if (line.find("rounded") != std::string::npos) st.rounded++;
  return line;
}

void runProperty(uint64_t seed, size_t count, bool big, Stats& st) {
  for (size_t i = 0; i < count; i++) {
    Gen g(seed * 1000003u + i);
    g.big = big;
    const int rootId = g.root(1 + (int)g.r(big ? 4 : 5));
    g.space();
    g.render(rootId);
    g.space();
    if (g.p(5)) g.out += "=";
    if (g.out.find('%') != std::string::npos) st.pct++;
    if (g.out.find('\xC3') != std::string::npos || g.out.find('\xE2') != std::string::npos) st.unicode++;
    const std::string want = expected(g, rootId, st);
    const std::string got = run(g.out);
    if (got != want) {
      char msg[1024];
      std::snprintf(msg, sizeof msg, "case %zu input [%s]: got [%s] want [%s]", i, g.out.c_str(),
                    got.c_str(), want.c_str());
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

void test_property_against_the_big_reference() {
  Stats st;
  runProperty(1, 90000, false, st);
  runProperty(2, 10000, true, st);
  std::printf("property: 100000 expressions: %zu figures (%zu rounded), %zu division by zero, "
              "%zu overflow, %zu over the length limit; %zu with a percent, %zu with typed signs\n",
              st.ok, st.rounded, st.divZero, st.overflow, st.tooLong, st.pct, st.unicode);
  // The generator covers every outcome class.
  TEST_ASSERT_TRUE(st.ok > 60000);
  TEST_ASSERT_TRUE(st.rounded > 1000);
  TEST_ASSERT_TRUE(st.divZero > 1000);
  TEST_ASSERT_TRUE(st.overflow > 50);
  TEST_ASSERT_TRUE(st.tooLong > 10);
  TEST_ASSERT_TRUE(st.pct > 5000);
  TEST_ASSERT_TRUE(st.unicode > 5000);
}

// Arbitrary bytes: one line, inside the cap, printable ASCII, a position inside the input.
void checkShape(const std::string& in) {
  const calc::Result r = evaluate(in);
  TEST_ASSERT_TRUE(r.len < calc::kLineMax);
  TEST_ASSERT_EQUAL(0, (int)r.line[r.len]);
  for (size_t i = 0; i < r.len; i++) {
    const unsigned char c = (unsigned char)r.line[i];
    TEST_ASSERT_TRUE_MESSAGE(c >= 0x20 && c < 0x7F, in.c_str());
  }
  const std::string t(r.text());
  if (r.ok()) {
    TEST_ASSERT_TRUE_MESSAGE(t.rfind("= ", 0) == 0, in.c_str());
    TEST_ASSERT_EQUAL(0, r.position);
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(t.rfind("error at position ", 0) == 0, in.c_str());
  TEST_ASSERT_TRUE(r.position >= 1 && r.position <= charPos(in, in.size()));
  TEST_ASSERT_EQUAL_STRING(("error at position " + std::to_string(r.position) + ": ").c_str(),
                           t.substr(0, t.find(": ") + 2).c_str());
}

void test_arbitrary_bytes_give_one_well_formed_line() {
  std::mt19937_64 rng(77);
  const char alphabet[] = "0123456789.,+-*/()% ofOF=^x$\t";
  for (int i = 0; i < 100000; i++) {
    std::string s;
    const size_t len = rng() % 40;
    for (size_t k = 0; k < len; k++) {
      if (rng() % 10 == 0) s.push_back((char)(rng() % 256));
      else s.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
    }
    checkShape(s);
  }
  for (int i = 0; i < 2000; i++) {
    std::string s(calc::kMaxExpr - (rng() % 3), '(');
    if (rng() % 2) s = std::string(calc::kMaxExpr, '9');
    checkShape(s);
  }
}

// No heap: counted, and refused, the bytes are the same.
void test_no_heap_and_a_refusing_allocator_changes_nothing() {
  std::vector<std::string> inputs;
  for (const Case& c : kAccepted) inputs.push_back(c.in);
  for (const Case& c : kRefused) inputs.push_back(c.in);
  std::vector<std::string> plain;
  for (const std::string& s : inputs) plain.push_back(run(s));
  std::vector<calc::Result> refused;
  refused.reserve(inputs.size());
  g_allocs = 0;
  g_counting = true;
  g_refuse = true;
  for (const std::string& s : inputs) refused.push_back(evaluate(s));
  g_counting = false;
  g_refuse = false;
  TEST_ASSERT_EQUAL(0, g_allocs);
  for (size_t i = 0; i < inputs.size(); i++)
    TEST_ASSERT_EQUAL_STRING(plain[i].c_str(), std::string(refused[i].text()).c_str());
}

void test_same_bytes_in_give_the_same_bytes_out() {
  for (const Case& c : kAccepted) {
    const calc::Result a = evaluate(c.in), b = evaluate(c.in);
    TEST_ASSERT_EQUAL(0, std::memcmp(&a.line, &b.line, sizeof a.line));
  }
}

void test_argument_names_lead_with_the_schema_name() {
  TEST_ASSERT_EQUAL_STRING("expression", calc::kArgNames[0]);
  TEST_ASSERT_EQUAL(8u, calc::kArgNameCount);
  TEST_ASSERT_EQUAL_STRING("calc", calc::kName);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_accepted_forms_give_the_exact_figure);
  RUN_TEST(test_refused_forms_give_the_exact_error_line);
  RUN_TEST(test_result_fields_match_the_line);
  RUN_TEST(test_every_error_value_is_reached);
  RUN_TEST(test_length_limit_is_exact);
  RUN_TEST(test_nesting_is_bounded);
  RUN_TEST(test_property_against_the_big_reference);
  RUN_TEST(test_arbitrary_bytes_give_one_well_formed_line);
  RUN_TEST(test_no_heap_and_a_refusing_allocator_changes_nothing);
  RUN_TEST(test_same_bytes_in_give_the_same_bytes_out);
  RUN_TEST(test_argument_names_lead_with_the_schema_name);
  return UNITY_END();
}
