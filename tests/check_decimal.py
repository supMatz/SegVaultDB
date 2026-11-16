#!/usr/bin/env python3
"""Checks tests/test_decimal.c output with Python's decimal module and MySQL's rules (P1.9).

Each line is  op|a|b|k|result .  Expected values:
  + -   exact, scale = max(scale a, scale b)
  *     exact, scale = scale a + scale b (at most 30)
  /     rounded half away from zero to scale a + 4 (div_precision_increment); integer / integer stays a
        double in SegVault (printed with 15 significant digits), a deliberate difference from MySQL
  ROUND(a, k)   half away from zero to k digits
  <     1 or 0
  SUM   the exact sum of three DECIMAL(30, 8) values, scale 8
"""
import sys
from decimal import Decimal, ROUND_HALF_UP, getcontext

getcontext().prec = 200


def scale(t):
    return len(t.split(".")[1]) if "." in t else 0


def fixed(d, sc):
    q = d.quantize(Decimal(1).scaleb(-sc), rounding=ROUND_HALF_UP)
    s = format(q, "f")
    if s.startswith("-") and Decimal(s) == 0:
        s = s[1:]
    return s


INT64 = (-(1 << 63), (1 << 63) - 1)


def expected(op, a, b, k):
    A = Decimal(a)
    if op == "ROUND":                       # un intero resta intero (ROUND(150, 2) = 150), un decimale ha k cifre
        return a if "." not in a else fixed(A, k)
    if op == "SUM":
        vals = [Decimal(x) for x in [a] + b.split(";")]
        return fixed(sum(fixed_d(v, 8) for v in vals), 8)
    B = Decimal(b)
    if "." not in a and "." not in b and op in "+-*":   # due interi: aritmetica a 64 bit, oltre è un errore (come MySQL)
        r = int(a) + int(b) if op == "+" else int(a) - int(b) if op == "-" else int(a) * int(b)
        if not INT64[0] <= r <= INT64[1]:
            return "ERR Integer result out of range (64 bit)"
    if op == "<":
        return "1" if A < B else "0"
    if op == "+":
        return fixed(A + B, max(scale(a), scale(b)))
    if op == "-":
        return fixed(A - B, max(scale(a), scale(b)))
    if op == "*":
        return fixed(A * B, min(scale(a) + scale(b), 30))
    if op == "/":
        if "." not in a and "." not in b:   # due interi: SegVault resta nel double ("%.15g"), MySQL darebbe scala 4
            return "%.15g" % (int(a) / int(b))
        return fixed(A / B, min(scale(a) + 4, 30))
    raise ValueError(op)


def fixed_d(v, sc):
    return v.quantize(Decimal(1).scaleb(-sc), rounding=ROUND_HALF_UP)


def main(path):
    bad = n = 0
    with open(path) as f:
        for line in f:
            op, a, b, k, got = line.rstrip("\n").split("|", 4)
            n += 1
            want = expected(op, a, b, int(k))
            if got != want:
                bad += 1
                if bad <= 10:
                    print(f"DIFF {op}: a={a} b={b} k={k}: got {got}, expected {want}")
    if bad:
        print(f"decimal: {bad} of {n} expressions differ from Python's decimal")
        return 1
    print(f"OK: {n}/{n} decimal expressions match Python's decimal (MySQL scale rules)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
