#!/usr/bin/env python3
"""Factory reset must land where a blank card lands.

`Core/Src/mod_state.c` holds the compiled defaults: what the radio runs with
when `MOD_LoadSettings()` finds no settings record. `MOD_FactoryReset()` in
`Core/Src/mod_storage.c` used to be a second, hand-written copy of the same
values, which had drifted (a factory reset gave a different transmit
bandwidth than a fresh flash and left four settings untouched) -- and which
the reset at its end then discarded anyway.

The reset now erases the settings record and the calibration and restarts,
so the next boot starts from exactly those compiled defaults. This checks
that sequence: the erases, the restart as the last statement, and no
setting assigned on the way (it could only mislead).
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

from validation_support import ROOT, ValidationError

DEFAULT_RE = re.compile(
    r"^\s*(?:volatile\s+)?[A-Za-z_][\w \t*]*?\b(cfg_\w+)\s*=\s*([^;]+);", re.M
)
ASSIGN_RE = re.compile(r"\b(cfg_\w+)\s*=\s*([^;]+);")
RESET_RE = re.compile(r"void MOD_FactoryReset\(void\)\s*\{(.*?)\n\}", re.S)
REQUIRED_CALLS = (
    "rf_en_force_idle()",
    "SD_StoreErase(SETTINGS_SLOT)",
    "cal_store_erase_all()",
)


def check() -> list[str]:
    state = (ROOT / "Core" / "Src" / "mod_state.c").read_text(encoding="utf-8")
    storage = (ROOT / "Core" / "Src" / "mod_storage.c").read_text(encoding="utf-8")

    if not DEFAULT_RE.findall(state):
        raise ValidationError("no cfg_* defaults found in mod_state.c")
    body_match = RESET_RE.search(storage)
    if body_match is None:
        raise ValidationError("MOD_FactoryReset() not found in mod_storage.c")
    body = body_match.group(1)
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)

    problems: list[str] = []
    for call in REQUIRED_CALLS:
        if call not in code:
            problems.append(f"MOD_FactoryReset does not call {call}")
    statements = [s.strip() for s in re.split(r"[;{}]", code) if s.strip()]
    if not statements or statements[-1] != "NVIC_SystemReset()":
        problems.append("MOD_FactoryReset must end with NVIC_SystemReset(): the boot after it "
                        "is what restores the compiled defaults")
    for name, _ in ASSIGN_RE.findall(code):
        problems.append(f"{name}: MOD_FactoryReset assigns a setting the restart discards")
    return problems


def main() -> int:
    try:
        problems = check()
    except ValidationError as error:
        print(f"FAIL  {error}")
        return 1
    if problems:
        for problem in problems:
            print(f"FAIL  {problem}")
        return 1
    print("PASS  MOD_FactoryReset() erases settings and calibration and restarts into "
          "the compiled defaults in mod_state.c")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
