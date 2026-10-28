#!/usr/bin/env python3
"""The console's pin table must match the document that defines the board.

`docs/rev1_pins.csv` is the machine-readable pin map: it is what the firmware
was written against and what a person reads when tracing a net. The USB
diagnostic console carries its own copy in `Core/Src/usb_diag_pins.c`, because
a `pins` query has to name each line without reading a file off the card.

Two copies drift. This compares them, so a pin that moves in the document
fails the build instead of quietly reporting the old net name to a host that
is trying to find a fault.
"""
from __future__ import annotations

import csv
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError

PORTS = "ABCDEFG"


def document_pins() -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    with (ROOT / "docs" / "rev1_pins.csv").open(encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            gpio = (row["gpio"] or "").strip()
            if not re.fullmatch(r"P[A-G]\d{1,2}", gpio):
                continue
            label = (row["label"] or "").strip().strip('"')
            label = label.split(",")[0].split("(")[0].strip()[:18].strip() or "-"
            signal = ((row["signal"] or "").strip().strip('"'))[:16].strip() or "-"
            result[gpio] = (label, signal)
    return result


def table_pins() -> dict[str, tuple[str, str]]:
    text = (ROOT / "Core" / "Src" / "usb_diag_pins.c").read_text(encoding="utf-8")
    body = text[text.index("diag_pins[] = {"):]
    result: dict[str, tuple[str, str]] = {}
    for port, pin, label, signal in re.findall(
            r'\{\s*(\d+),\s*(\d+),\s*"([^"]*)",\s*"([^"]*)"\s*\}', body):
        result[f"P{PORTS[int(port)]}{int(pin)}"] = (label, signal)
    return result


def analyze() -> dict[str, object]:
    document = document_pins()
    table = table_pins()
    if not document or not table:
        raise ValidationError("pin map or console pin table is empty")
    missing = sorted(set(document) - set(table))
    extra = sorted(set(table) - set(document))
    differing = sorted(p for p in set(document) & set(table) if document[p] != table[p])
    problems = []
    if missing:
        problems.append("absent from the console table: " + ", ".join(missing))
    if extra:
        problems.append("not in the pin map: " + ", ".join(extra))
    for pin in differing:
        problems.append(f"{pin}: map has {document[pin]}, console has {table[pin]}")
    if problems:
        raise ValidationError("; ".join(problems))
    return {"pins": len(table)}


def main() -> int:
    try:
        result = analyze()
    except ValidationError as error:
        print(f"FAIL  {error}")
        return 1
    print(f"PASS  console pin table matches docs/rev1_pins.csv for all {result['pins']} pins")
    return 0


if __name__ == "__main__":
    sys.exit(main())
