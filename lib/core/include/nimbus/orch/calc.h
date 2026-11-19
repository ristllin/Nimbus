#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// calc - exact decimal arithmetic on one expression (lane RB-REC8, ruling 42 item 3).
//
// A figure the person asks for ("what is left against the target", "this amount scaled to
// that one") comes from this tool, not from a model's head. One expression in, one plain
// line out: the exact result, or an error that names the position. Never a wrong number.
//
// Arithmetic is exact: every value is a reduced fraction of two unsigned 128-bit integers
// (numerator and denominator below 2^124) with a sign, so 0.1+0.2 is 0.3 and 100/3*3 is 100.
// A value that would leave that range is the Overflow error, never a rounded figure.
// No heap: the evaluator lives on the caller's stack and the result line is inside Result
// (a refusing allocator changes nothing). Recursion happens only at '(' and is bounded by
// kMaxDepth. Portable, Arduino-free, host-tested (test/test_calc).
//
// Accepted input (at most kMaxExpr bytes, UTF-8):
//   numbers     12  12.5  .5  5.  007; thousands separators and decimal commas as people
//               type them, read one number at a time:
//                 1,234.56 and 1.234,56  -> 1234.56 (both marks: the last one is decimal)
//                 1,234,567 and 1.234.567 -> 1234567 (a mark twice: thousands)
//                 1.234                  -> 1.234 (a single '.' is always a decimal point)
//                 1,5  12,25  0,250  1234,5 -> decimal comma (cannot be thousands)
//                 1,234                  -> refused: could be 1234 or 1.234
//               Thousands groups are exactly 3 digits after a first group of 1 to 3 digits
//               that does not start with 0; anything else is refused. At most kMaxDigits
//               digits before and after the point (leading zeros before, trailing zeros
//               after, are not counted). No spaces, apostrophes or underscores inside a number.
//   operators   + - * / and the typed forms U+00D7 (times), U+00F7 (divide),
//               U+2212 (minus); parentheses; unary minus and plus (repeatable).
//               No implicit multiplication: 2(3) is an error. '^' and '**' are refused.
//   percent     N% is N/100, with these readings (decided, tested):
//                 15%            -> 0.15 (the whole expression, or a factor: 200*15% is 30)
//                 15% of 80      -> 12 ('of' takes the next number or parenthesis; chains)
//                 80+15%, 80-15% -> 92, 68: a bare N% as the right operand of + or -
//                                   is N percent of the running total on its left
//                                   (100+10+5% is 115.5)
//                 (15%)          -> 0.15: parentheses make a percent an ordinary number
//               Refused as unclear: any other percent that is an operand of + or -
//               (15%+5, 50%+50%, 80+2*15%, 80+-15%, 80+50% of 20%); '%' not after a
//               number; 'of' not after a percent.
//   spaces      spaces, tabs and line breaks between tokens; one trailing '=' is ignored.
//
// The result line (one of):
//   "= 687.5"                                  exact: whole digits, no separators, no
//                                              exponent, no trailing zeros, at most
//                                              kExactPlaces decimal places
//   "= 0.333333 (rounded to 6 decimal places)" the exact value has more decimal places
//                                              (or never ends); rounded half away from
//                                              zero to kRoundedPlaces, trailing zeros
//                                              trimmed; a value that rounds to zero is "0"
//   "error at position N: <reason>"            N is the 1-based character position
// The error reasons, exactly (Error, then the text after "error at position N: "):
//   Empty             empty expression
//   TooLong           longer than 256 bytes
//   BadChar           unexpected 'X'            (X the character; control bytes and broken
//                                                UTF-8 read "unexpected byte")
//   UnknownWord       unknown word 'W': use numbers, + - * / ( ) % and of
//                                               (W at most 12 bytes)
//   Unsupported       'X' is not supported: use + - * / ( ) %
//   ExpectedValue     expected a number or '('
//   ExpectedOperator  expected an operator or ')'
//   MissingClose      missing ')'               (position: the '(' left open)
//   ExtraClose        ')' without '('
//   TooDeep           more than 10 nested parentheses
//   NumberTooLong     more than 18 digits before or after the point
//   BadNumber         number not understood: use digits with one '.' for decimals
//   UnclearNumber     'T' could be A or B: write one of them
//                     (T the number as typed, A its thousands reading, B its decimal
//                     reading; T longer than 24 bytes reads "this number")
//   PercentPlacement  '%' must follow a number
//   OfPlacement       'of' must follow a percent, as in 15% of 80
//   UnclearPercent    unclear percent: write N% of X, or put it in parentheses
//   DivByZero         division by zero          (position: the '/')
//   Overflow          too large to compute exactly (position: the operator)
//
// Tool declaration proposed for phase 2 (the bytes move to plain/texts.h there):
//   name        calc
//   description "Exact decimal arithmetic on one expression: + - * / ( ), 15% of 80, 80+15%."
//               (75 B)
//   schema      {"type":"object","properties":{"expression":{"type":"string","maxLength":256}},"required":["expression"]}
//               (105 B)
//   arguments   the first present of kArgNames, as a string (the names models reach for)
namespace nimbus::orch::calc {

inline constexpr size_t kMaxExpr = 256;         // bytes of input
inline constexpr unsigned kMaxDepth = 10;       // nested parentheses
inline constexpr unsigned kMaxDigits = 18;      // digits before, and after, the point
inline constexpr unsigned kExactPlaces = 12;    // exact results show up to this many places
inline constexpr unsigned kRoundedPlaces = 6;   // longer results are rounded to this many
inline constexpr size_t kLineMax = 128;         // the longest result line, NUL included

inline constexpr char kName[] = "calc";
// The argument names accepted for the expression, schema name first. A binding takes the
// first one present whose value is a string.
inline constexpr const char* kArgNames[] = {"expression", "expr",    "input", "query",
                                            "formula",    "calculation", "math",  "text"};
inline constexpr size_t kArgNameCount = sizeof(kArgNames) / sizeof(kArgNames[0]);

enum class Error : uint8_t {
  None = 0,
  Empty,
  TooLong,
  BadChar,
  UnknownWord,
  Unsupported,
  ExpectedValue,
  ExpectedOperator,
  MissingClose,
  ExtraClose,
  TooDeep,
  NumberTooLong,
  BadNumber,
  UnclearNumber,
  PercentPlacement,
  OfPlacement,
  UnclearPercent,
  DivByZero,
  Overflow,
};

struct Result {
  Error error = Error::None;
  uint16_t position = 0;   // 1-based character position of the error; 0 when ok
  bool rounded = false;    // ok, and the line shows a rounded value
  uint8_t len = 0;         // bytes of line, NUL excluded
  char line[kLineMax] = {};
  bool ok() const { return error == Error::None; }
  std::string_view text() const { return std::string_view(line, len); }
};

// Evaluates one expression. Pure: the same bytes in give the same bytes out, on every build.
Result evaluate(std::string_view expression);

}  // namespace nimbus::orch::calc
