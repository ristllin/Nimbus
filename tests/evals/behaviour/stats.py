#!/usr/bin/env python3
"""stats - exact one-sided 95% binomial bounds (Clopper-Pearson).

upper(0, 300) = 0.0099 and upper(2, 630) = 0.0099: the thresholds PRD section 8 quotes for shadow mode.
"""

from __future__ import annotations

from math import comb


def _cdf(k: int, n: int, p: float) -> float:
    """P(X <= k) for X ~ Binomial(n, p)."""
    if p <= 0:
        return 1.0
    if p >= 1:
        return 1.0 if k >= n else 0.0
    return min(1.0, sum(comb(n, i) * p**i * (1 - p) ** (n - i) for i in range(k + 1)))


def _solve(f, lo=0.0, hi=1.0) -> float:
    """The p in [lo, hi] where the decreasing function f(p) crosses 0.05."""
    for _ in range(80):
        mid = (lo + hi) / 2
        if f(mid) > 0.05:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def upper(k: int, n: int) -> float:
    """One-sided 95% upper bound on a rate seen k times in n."""
    if n == 0:
        return 1.0
    if k >= n:
        return 1.0
    return _solve(lambda p: _cdf(k, n, p))


def lower(k: int, n: int) -> float:
    """One-sided 95% lower bound on a rate seen k times in n."""
    if n == 0 or k == 0:
        return 0.0
    return 1 - upper(n - k, n)


def rate(k: int, n: int, side: str = "upper") -> dict:
    out = {"k": k, "n": n, "rate": round(k / n, 4) if n else None}
    out["upper95" if side == "upper" else "lower95"] = round(upper(k, n) if side == "upper" else lower(k, n), 4)
    return out


def fmt(r: dict) -> str:
    if not r["n"]:
        return "n=0"
    b = r.get("upper95", r.get("lower95"))
    side = "<=" if "upper95" in r else ">="
    return f"{r['k']}/{r['n']} = {100 * r['rate']:.1f}% ({side} {100 * b:.1f}%)"
