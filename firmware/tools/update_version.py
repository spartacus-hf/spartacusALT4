#!/usr/bin/env python3
"""Transactionally update canonical firmware-version representations."""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import REPO, ROOT, ValidationError
from version_tool import HEADER, README, REPO_README, collect_failures

VERSION_RE = re.compile(r"^v([0-9]+)\.([0-9]+)$")


def substitute_once(text: str, pattern: str, replacement: str, label: str) -> str:
    result, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise ValidationError(f"cannot uniquely update {label}")
    return result


def atomic_replace_bytes(path: Path, content: bytes, mode: int) -> None:
    handle, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temp_path = Path(temporary)
    try:
        os.fchmod(handle, mode & 0o777)
        with os.fdopen(handle, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temp_path, path)
    finally:
        temp_path.unlink(missing_ok=True)


def set_version(value: str) -> tuple[str, list[str], dict[str, object]]:
    match = VERSION_RE.fullmatch(value)
    if match is None:
        raise ValidationError("version must use vMAJOR.MINOR, for example v6.79")
    major, minor = int(match.group(1)), int(match.group(2))
    if major > 9 or minor > 99:
        raise ValidationError("version must fit USB/SCSI firmware fields: major 0-9, minor 0-99")

    replacements: dict[Path, str] = {}
    header = HEADER.read_text(encoding="utf-8")
    header = substitute_once(header, r"^(\s*#define\s+FW_VERSION_MAJOR\s+)[0-9]+\s*$",
                             rf"\g<1>{major}", "FW_VERSION_MAJOR")
    header = substitute_once(header, r"^(\s*#define\s+FW_VERSION_MINOR\s+)[0-9]+\s*$",
                             rf"\g<1>{minor}", "FW_VERSION_MINOR")
    replacements[HEADER] = header
    replacements[ROOT / "VERSION"] = value + "\n"
    replacements[README] = substitute_once(
        README.read_text(encoding="utf-8"), r"^Current firmware:\s*`v[0-9]+\.[0-9]+`\.$",
        f"Current firmware: `{value}`.", "alt2 README current-version line")
    replacements[REPO_README] = substitute_once(
        REPO_README.read_text(encoding="utf-8"),
        r"^The current firmware version is\s*`v[0-9]+\.[0-9]+`\.$",
        f"The current firmware version is `{value}`.", "repository README current-version line")

    for path in replacements:
        if path.is_symlink():
            raise ValidationError(f"refusing to update symlink: {path}")
    originals = {path: (path.read_bytes(), path.stat().st_mode) for path in replacements}
    replaced: list[Path] = []
    try:
        for path, content in replacements.items():
            encoded = content.encode("utf-8")
            if encoded == originals[path][0]:
                continue
            atomic_replace_bytes(path, encoded, originals[path][1])
            replaced.append(path)
        version, failures, details = collect_failures()
        if failures:
            raise ValidationError("post-update version check failed: " + "; ".join(failures))
        return version, failures, details
    except Exception:
        rollback_errors: list[str] = []
        for path in reversed(replaced):
            try:
                content, mode = originals[path]
                atomic_replace_bytes(path, content, mode)
            except OSError as rollback_error:
                rollback_errors.append(f"{path}: {rollback_error}")
        if rollback_errors:
            raise ValidationError("version update failed and rollback was incomplete: "
                                  + "; ".join(rollback_errors))
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="new vMAJOR.MINOR version")
    args = parser.parse_args()
    try:
        version, failures, _ = set_version(args.version)
    except (OSError, ValidationError) as exc:
        print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if failures:
        for failure in failures:
            print(f"FAIL  {failure}")
        return 1
    print(f"PASS  updated firmware version representations to {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
