"""Exact-text patching for c1's sources, keeping each file's line endings (most of c1 is CRLF)."""
import os

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "c1", "src")


def patch(rel, pairs):
    """pairs: (old, new, expected_count). Fails without writing if any count is off."""
    p = os.path.join(ROOT, *rel.split("/"))
    raw = open(p, newline="").read()
    crlf = raw.count("\r\n") > raw.count("\n") // 2
    s = raw.replace("\r\n", "\n")
    for old, new, count in pairs:
        n = s.count(old)
        if n != count:
            raise SystemExit(f"{rel}: expected {count} of {old[:70]!r}, found {n}")
        s = s.replace(old, new)
    open(p, "w", newline="").write(s.replace("\n", "\r\n") if crlf else s)
    print("patched", rel)
