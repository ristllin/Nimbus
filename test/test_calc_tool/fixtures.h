#pragma once
// The calc tool's shared fixtures: test_calc_tool (old engine against engine 2) and
// nimbusd's test_rig (the hosted registry) call calc with each and expect the bytes
// calc::evaluate gives.
namespace calc_fixtures {

inline constexpr const char* kExpressions[] = {
    "0.1+0.2", "2300-1612.5", "250*3.12/100", "1/3", "15% of 80", "80+15%", "1,234",
    "1/0", "2 3", "(2+3", "sqrt(4)", "", "999999999999999999*999999999999999999*100",
};

}  // namespace calc_fixtures
