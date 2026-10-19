"""Host tests for check_panic_actions.py (no board, no toolchain).

The bad fixtures are the shapes GCC really emitted for the armed faults on 19 Oct: the
recursion compiled into one frame and a back-jump, and the null store gone. The good
fixtures are the shapes the fixed functions must keep.

Run: python3 -m pytest tools/release_gate
"""

from __future__ import annotations

import check_panic_actions as pa

GOOD_NULL = """
421ad900 <nimbus::crash::(anonymous namespace)::faultNullStore()>:
421ad900:	004136               	entry	a1, 32
421ad903:	6fe681               	l32r	a8, 421897ec <g_faultAddr>
421ad906:	0020c0               	memw
421ad909:	0888                	l32i.n	a8, a8, 0
421ad90b:	6fe591               	l32r	a9, 421897f0 <kMagic>
421ad90e:	0020c0               	memw
421ad911:	0899                	s32i.n	a9, a8, 0
421ad913:	f01d                	retw.n
"""

GOOD_DEEPEN = """
421ac094 <nimbus::crash::(anonymous namespace)::faultDeepen(unsigned long)>:
421ac094:	046136               	entry	a1, 0x230
421ac097:	0261a2               	s32i	a10, a1, 8
421ac0c0:	2128                	l32i.n	a2, a1, 8
421ac0c2:	a21b                	addi.n	a10, a2, 1
421ac0c4:	fffce5               	call8	421ac094 <nimbus::crash::(anonymous namespace)::faultDeepen(unsigned long)>
421ac0c7:	f01d                	retw.n
"""

# What GCC emitted on 19 Oct: one 560 B frame, fill the pad, jump back (no self-call).
LOOPED_DEEPEN = """
421ac094 <nimbus::crash::(anonymous namespace)::faultDeepen(unsigned long)>:
421ac094:	046136               	entry	a1, 0x230
421ac0ad:	0a0c                	movi.n	a10, 0
421ac0af:	00a2b2               	movi	a11, 0x200
421ac0b2:	0e8b76               	loop	a11, 421ac0c4 <faultDeepen+0x30>
421ac0bf:	004dc2               	s8i	a12, a13, 0
421ac0cf:	881b                	addi.n	a8, a8, 1
421ac0d1:	fff606               	j	421ac0ad <faultDeepen+0x19>
"""

# A store-free body (the store deleted, the function left as an empty shell).
EMPTY_NULL = """
421ad900 <nimbus::crash::(anonymous namespace)::faultNullStore()>:
421ad900:	004136               	entry	a1, 32
421ad903:	f01d                	retw.n
"""

# A call to some other function is not a self-call.
OTHER_CALL_DEEPEN = """
421ac094 <nimbus::crash::(anonymous namespace)::faultDeepen(unsigned long)>:
421ac094:	046136               	entry	a1, 0x230
421ac0c4:	fffce5               	call8	421bc88c <Print::printf(char const*, ...)>
421ac0c7:	f01d                	retw.n
"""


def _judge(*parts: str) -> "list[str]":
    return pa.judge(pa.functions("\n".join(parts)))


def test_real_faults_pass():
    assert _judge(GOOD_NULL, GOOD_DEEPEN) == []


def test_looped_recursion_fails():
    problems = _judge(GOOD_NULL, LOOPED_DEEPEN)
    assert len(problems) == 1 and "never calls itself" in problems[0]


def test_a_call_elsewhere_is_not_recursion():
    problems = _judge(GOOD_NULL, OTHER_CALL_DEEPEN)
    assert len(problems) == 1 and "never calls itself" in problems[0]


def test_deleted_null_store_fails():
    problems = _judge(EMPTY_NULL, GOOD_DEEPEN)
    assert len(problems) == 1 and "optimized away" in problems[0]


def test_missing_functions_fail():
    problems = _judge("")
    assert len(problems) == 2 and all("no single" in p for p in problems)
    # the 19 Oct image: no null-store function at all and the looped recursion
    problems = _judge(LOOPED_DEEPEN)
    assert any("no single faultNullStore" in p for p in problems)
    assert any("never calls itself" in p for p in problems)


def test_functions_parser_keeps_start_and_body():
    funcs = pa.functions(GOOD_DEEPEN)
    ((name, (start, body)),) = funcs.items()
    assert "faultDeepen" in name and start == 0x421AC094 and len(body) == 6
