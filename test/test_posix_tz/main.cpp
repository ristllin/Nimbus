#include <unity.h>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "nimbus/orch/posix_tz.h"

// QW5: a timezone rule the device's C library cannot parse is silently treated as
// UTC, so a typo saved from Settings or the setup wizard left routines and food-log
// days on UTC while the page said "Saved". POST /api/config now refuses it
// (posixTzError). The class: every rule the page derives from a browser, and every
// suggestion the page offers, is accepted; malformed rules are refused.

using nimbus::orch::posixTzError;

void setUp() {}
void tearDown() {}

// tzFromBrowser() output for 41 zones (Chromium, 2026; lanes/QW5/evidence/
// tz-semantic-check.txt checks each against the IANA zone hour by hour).
static const char* const kDerived[] = {
    "UTC0",   // UTC
    "GMT0BST,M3.5.0/1,M10.5.0",   // Europe/London
    "CET-1CEST,M3.5.0,M10.5.0/3",   // Europe/Paris
    "CET-1CEST,M3.5.0,M10.5.0/3",   // Europe/Berlin
    "EET-2EEST,M3.5.0/3,M10.5.0/4",   // Europe/Helsinki
    "<+03>-3",   // Europe/Moscow
    "EST5EDT,M3.2.0,M11.1.0",   // America/New_York
    "CST6CDT,M3.2.0,M11.1.0",   // America/Chicago
    "MST7MDT,M3.2.0,M11.1.0",   // America/Denver
    "MST7",   // America/Phoenix
    "PST8PDT,M3.2.0,M11.1.0",   // America/Los_Angeles
    "AKST9AKDT,M3.2.0,M11.1.0",   // America/Anchorage
    "HST10",   // Pacific/Honolulu
    "<-0330>3:30<-0230>,M3.2.0,M11.1.0",   // America/St_Johns
    "<-03>3",   // America/Sao_Paulo
    "<-04>4<-03>,M9.1.6/24,M4.1.6/24",   // America/Santiago
    "CST6",   // America/Mexico_City
    "<+0530>-5:30",   // Asia/Kolkata
    "<+0545>-5:45",   // Asia/Kathmandu
    "<+09>-9",   // Asia/Tokyo
    "<+08>-8",   // Asia/Shanghai
    "<+02>-2<+03>,M3.4.4/26,M10.5.0",   // Asia/Jerusalem
    "<+0330>-3:30",   // Asia/Tehran
    "<+10>-10<+11>,M10.1.0,M4.1.0/3",   // Australia/Sydney
    "<+0930>-9:30<+1030>,M10.1.0,M4.1.0/3",   // Australia/Adelaide
    "<+10>-10",   // Australia/Brisbane
    "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0",   // Australia/Lord_Howe
    "<+12>-12<+13>,M9.5.0,M4.1.0/3",   // Pacific/Auckland
    "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45",   // Pacific/Chatham
    "EET-2EEST,M4.5.5/0,M10.5.4/24",   // Africa/Cairo
    "<+01>-1",   // Africa/Casablanca
    "<-02>2<-01>,M3.5.6/23,M10.5.0/0",   // America/Nuuk
    "<-01>1<+00>,M3.5.0/0,M10.5.0/1",   // Atlantic/Azores
    "GST-4",   // Asia/Dubai
    "<-03>3",   // America/Asuncion
    "GMT0<+01>,M3.5.0/1,M10.5.0",   // Europe/Dublin
    "<-05>5<-04>,M3.2.0/0,M11.1.0/1",   // America/Havana
    "EET-2EEST,M3.5.6,M10.4.6",   // Asia/Gaza
    "<+12>-12",   // Pacific/Fiji
    "EST5EDT,M3.2.0,M11.1.0",   // America/Toronto
    "WET0WEST,M3.5.0/1,M10.5.0",   // Europe/Lisbon
};

static void test_every_rule_the_page_derives_is_accepted() {
  for (const char* tz : kDerived) TEST_ASSERT_EQUAL_STRING_MESSAGE("", posixTzError(tz).c_str(), tz);
}

// Every suggestion in the Settings list (include/web/ui_device.h <datalist id=tzlist>),
// read from the page itself so a new suggestion is checked with no new code.
static void test_every_suggestion_the_page_offers_is_accepted() {
  std::ifstream f("include/web/ui_device.h");
  TEST_ASSERT_TRUE_MESSAGE(f.good(), "include/web/ui_device.h");
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string page = ss.str();
  const size_t a = page.find("<datalist id=tzlist>"), b = page.find("</datalist>", a);
  TEST_ASSERT_TRUE(a != std::string::npos && b != std::string::npos);
  const std::string list = page.substr(a, b - a);
  const std::regex opt("<option value=\"([^\"]*)\">");
  int n = 0;
  for (std::sregex_iterator it(list.begin(), list.end(), opt), end; it != end; ++it, ++n)
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", posixTzError((*it)[1].str()).c_str(), (*it)[1].str().c_str());
  TEST_ASSERT_TRUE(n >= 8);
}

static void test_other_valid_forms() {
  static const char* const kOk[] = {"",          "UTC0",       "EST5EDT",      "EST5EDT4",
                                    "IST-5:30",  "XYZ+1:30:15", "EST5EDT,J60/2,J300",
                                    "EST5EDT,59,300", "<-03>3<-02>,M3.2.0/0:30,M11.1.0/1:15:30",
                                    "IST-2IDT,M3.4.4/26,M10.5.0", "London5"};
  for (const char* tz : kOk) TEST_ASSERT_EQUAL_STRING_MESSAGE("", posixTzError(tz).c_str(), tz);
}

// Refused: a zone NAME (the device needs the rule), no offset, a half rule, numbers
// out of range, a 2-letter or unclosed quoted name, a negative change time, trailing
// characters, and anything longer than the 48-byte setting.
static void test_malformed_rules_are_refused() {
  static const char* const kBad[] = {"Europe/London",
                                     "EST",
                                     "AB1",
                                     "UTC+25",
                                     "GMT0BST,M3.5.0/1",
                                     "GMT0BST,M3.5.0/1,",
                                     "GMT0BST,M13.5.0,M10.5.0",
                                     "GMT0BST,M3.6.0,M10.5.0",
                                     "GMT0BST,M3.5.7,M10.5.0",
                                     "GMT0BST,J0,J300",
                                     "GMT0BST,M3.5.0/-1,M10.5.0",
                                     "GMT0BST,M3.5.0/168,M10.5.0",
                                     "<+9>-9",
                                     "<+09-9",
                                     "PST8PDT,M3.2.0,M11.1.0x",
                                     "GMT0 ",
                                     "GMT0:60"};
  for (const char* tz : kBad) {
    const std::string err = posixTzError(tz);
    TEST_ASSERT_FALSE_MESSAGE(err.empty(), tz);
    TEST_ASSERT_EQUAL_MESSAGE(0u, err.find("That timezone rule is not one the device can read."), tz);
  }
  TEST_ASSERT_EQUAL(0u, posixTzError(std::string(49, 'A')).find("That timezone rule is too long."));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_rule_the_page_derives_is_accepted);
  RUN_TEST(test_every_suggestion_the_page_offers_is_accepted);
  RUN_TEST(test_other_valid_forms);
  RUN_TEST(test_malformed_rules_are_refused);
  return UNITY_END();
}
