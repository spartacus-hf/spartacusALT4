#!/usr/bin/env python3
"""Focused regression tests for validation/disassembly parsing logic."""

from __future__ import annotations

import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from elf_analysis import is_indirect_call_instruction  # noqa: E402


def main() -> int:
    indirect = [
        " 8021496:\t47c0      \tblx\tr8",
        " 803f248:\t4718      \tbx\tr3",
        " 804300c:\t4718      \tbx\tip",
        " 8043104:\t47d0      \tblx\tsl",
    ]
    direct_or_return = [
        " 8001000:\tf000 f812 \tbl\t8001028 <target>",
        " 8001020:\t4770      \tbx\tlr",
        " 8001030:\te7fa      \tb.n\t8001028 <target>",
    ]
    for line in indirect:
        if not is_indirect_call_instruction(line):
            raise AssertionError(f"missed indirect call/tail-call: {line}")
    for line in direct_or_return:
        if is_indirect_call_instruction(line):
            raise AssertionError(f"misclassified instruction: {line}")
    print("PASS  ELF parser recognizes BLX and BX function-pointer calls without treating BX LR as a call")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
