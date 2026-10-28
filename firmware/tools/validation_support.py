#!/usr/bin/env python3
"""Shared, read-only support for alt2 validation and reporting tools."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
POLICY_PATH = ROOT / "tools" / "validation_policy.json"
VERSION_RE = re.compile(r"^v([0-9]+)\.([0-9]+)$")


class ValidationError(RuntimeError):
    """A validation failure with a user-facing message."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


def policy(build_dir: str | None = None) -> dict[str, object]:
    """The validation policy; for a build directory, as that build sees it.

    Functions listed under "deep_diagnostic_only" exist only in builds whose
    "deep_diagnostics" is true (they are compiled out elsewhere), so for any
    other build they are dropped from the real-time roots, the stack roots
    and the reviewed indirect targets. The deep build still has to link
    every one of them: the ELF and stack checks fail on a missing root.
    """
    try:
        settings = json.loads(POLICY_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValidationError(f"cannot read {POLICY_PATH.relative_to(ROOT)}: {exc}") from exc
    if build_dir is None:
        return settings
    deep = bool(settings.get("builds", {}).get(build_dir, {}).get("deep_diagnostics", False))
    only_deep = {str(name) for name in settings.get("deep_diagnostic_only", [])}
    if deep or not only_deep:
        return settings
    for key in ("realtime_roots", "realtime_stack_roots", "foreground_stack_roots"):
        if key in settings:
            settings[key] = [item for item in settings[key] if str(item) not in only_deep]
    if "reviewed_indirect_targets" in settings:
        settings["reviewed_indirect_targets"] = {
            caller: [target for target in targets if str(target) not in only_deep]
            for caller, targets in settings["reviewed_indirect_targets"].items()
        }
    return settings


def read_version() -> tuple[str, int, int]:
    try:
        value = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    except OSError as exc:
        raise ValidationError(f"cannot read VERSION: {exc}") from exc
    match = VERSION_RE.fullmatch(value)
    if match is None:
        raise ValidationError(f"VERSION must use vMAJOR.MINOR, found {value!r}")
    return value, int(match.group(1)), int(match.group(2))


def command_env() -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    tool_bin = stm32_tool_bin(required=False)
    if tool_bin is not None:
        env["PATH"] = str(tool_bin) + os.pathsep + env.get("PATH", "")
    return env


def run(
    command: Sequence[str | os.PathLike[str]],
    *,
    cwd: Path = ROOT,
    check: bool = True,
    capture: bool = True,
) -> CommandResult:
    argv = [os.fspath(item) for item in command]
    proc = subprocess.run(
        argv,
        cwd=cwd,
        env=command_env(),
        check=False,
        text=True,
        capture_output=capture,
    )
    result = CommandResult(proc.returncode, proc.stdout or "", proc.stderr or "")
    if check and proc.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic output"
        raise ValidationError(f"command failed ({proc.returncode}): {' '.join(argv)}\n{detail}")
    return result


def stm32_tool_bin(*, required: bool = True) -> Path | None:
    compiler = shutil.which("arm-none-eabi-gcc")
    if compiler is not None:
        return Path(compiler).resolve().parent
    candidates = sorted(
        Path("/opt/stm32cubeide/plugins").glob(
            "com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.*/tools/bin"
        )
    )
    for candidate in reversed(candidates):
        if (candidate / "arm-none-eabi-gcc").is_file():
            return candidate
    if required:
        raise ValidationError("STM32 GNU toolchain not found in PATH or STM32CubeIDE")
    return None


def stm32_tool(name: str, *, required: bool = True) -> Path | None:
    tool_bin = stm32_tool_bin(required=required)
    if tool_bin is None:
        return None
    tool = tool_bin / f"arm-none-eabi-{name}"
    if tool.is_file():
        return tool
    fallback = shutil.which(f"arm-none-eabi-{name}")
    if fallback is not None:
        return Path(fallback)
    if required:
        raise ValidationError(f"arm-none-eabi-{name} not found")
    return None


def host_cc() -> Path:
    compiler = shutil.which("gcc") or shutil.which("cc")
    if compiler is None:
        raise ValidationError("host C compiler not found")
    return Path(compiler)


def build_artifacts(build_name: str) -> tuple[Path, Path]:
    build = ROOT / build_name
    return build / "alt2.elf", build / "alt2.map"


def require_fresh_build(build_name: str) -> Path:
    elf, map_path = build_artifacts(build_name)
    if not elf.is_file() or not map_path.is_file():
        raise ValidationError(
            f"{build_name} build artifacts are missing; build {build_name}/alt2.elf first"
        )
    result = run(["make", "-q", "-C", build_name, "alt2.elf"], check=False)
    if result.returncode == 1:
        raise ValidationError(f"{build_name}/alt2.elf is stale; rebuild it before validation")
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ValidationError(f"cannot verify {build_name} freshness: {detail}")
    return elf


def tracked_snapshot() -> dict[str, str]:
    """Hash tracked files so a validator can prove it did not mutate sources."""
    listing = run(["git", "ls-files", "-z"], cwd=REPO).stdout
    snapshot: dict[str, str] = {}
    for rel in listing.split("\0"):
        if not rel:
            continue
        path = REPO / rel
        if not path.is_file():
            snapshot[rel] = "<missing>"
            continue
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        snapshot[rel] = digest.hexdigest()
    return snapshot


def assert_snapshot_unchanged(before: dict[str, str]) -> None:
    after = tracked_snapshot()
    changed = sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))
    if changed:
        preview = ", ".join(changed[:8])
        suffix = "" if len(changed) <= 8 else f" and {len(changed) - 8} more"
        raise ValidationError(f"validation modified tracked files: {preview}{suffix}")


def source_tree_snapshot() -> dict[str, str]:
    """Hash source/configuration inputs, including untracked files."""
    excluded_parts = {
        ".git", "Debug", "Release-Speed", "Diagnostic", "_archive", "__pycache__",
    }
    # The radio bench's output (git-ignored): a bench run or its openocd may
    # write there while validation runs.
    excluded_dirs = ((ROOT.relative_to(REPO) / "tools" / "bench" / "results").as_posix(),)
    snapshot: dict[str, str] = {}
    for path in REPO.rglob("*"):
        relative = path.relative_to(REPO)
        generated_part = any(
            part in excluded_parts
            or part.startswith((".Release-Speed.tmp.", ".Diagnostic.tmp."))
            for part in relative.parts
        ) or any(relative.as_posix().startswith(d + "/") for d in excluded_dirs)
        if not path.is_file() or generated_part:
            continue
        snapshot[relative.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return snapshot


def assert_source_tree_unchanged(before: dict[str, str]) -> None:
    after = source_tree_snapshot()
    changed = sorted(key for key in before.keys() | after.keys() if before.get(key) != after.get(key))
    if changed:
        raise ValidationError("validation modified source/configuration files: " + format_list(changed))


def parse_nm_symbols(elf: Path) -> set[str]:
    nm = stm32_tool("nm")
    output = run([nm, "--defined-only", elf]).stdout
    symbols: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            symbols.add(fields[-1])
    return symbols


def read_elf_symbol_bytes(elf: Path, symbol: str) -> bytes:
    """Read an object's linked bytes by symbol and section without executing it."""
    objdump = stm32_tool("objdump")
    table = run([objdump, "-t", elf]).stdout
    matches: list[tuple[int, str, int]] = []
    for line in table.splitlines():
        fields = line.split()
        if len(fields) < 6 or fields[-1] != symbol:
            continue
        try:
            address = int(fields[0], 16)
            size = int(fields[-2], 16)
        except ValueError:
            continue
        matches.append((address, fields[-3], size))
    if len(matches) != 1:
        raise ValidationError(
            f"expected one linked object symbol {symbol}, found {len(matches)}")
    address, section, size = matches[0]
    if size == 0:
        raise ValidationError(f"linked object symbol {symbol} has zero size")

    dump = run([objdump, "-s", "-j", section, elf]).stdout
    memory: dict[int, int] = {}
    for line in dump.splitlines():
        fields = line.split()
        if len(fields) < 2 or re.fullmatch(r"[0-9a-fA-F]+", fields[0]) is None:
            continue
        cursor = int(fields[0], 16)
        for word in fields[1:]:
            if re.fullmatch(r"[0-9a-fA-F]{8}", word) is None:
                break
            for byte in bytes.fromhex(word):
                memory[cursor] = byte
                cursor += 1
    try:
        return bytes(memory[address + offset] for offset in range(size))
    except KeyError as exc:
        raise ValidationError(
            f"cannot recover all {size} bytes for linked symbol {symbol} from {section}") from exc


def source_files() -> set[str]:
    result: set[str] = set()
    for base in (ROOT / "Core" / "Src", ROOT / "USB_Device"):
        for path in base.rglob("*.c"):
            result.add(path.relative_to(ROOT).as_posix())
    return result


def metadata_sources() -> set[str]:
    result: set[str] = set()
    for mk in (ROOT / "Debug").rglob("subdir.mk"):
        text = mk.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"\.\./([^\\\s]+\.c)", text):
            source = match.group(1)
            if "%" not in source:
                result.add(source)
    return result


def print_pass(message: str) -> None:
    print(f"PASS  {message}")


def print_warn(message: str) -> None:
    print(f"WARN  {message}")


def fail(message: str) -> None:
    raise ValidationError(message)


def ensure_inside(path: Path, parent: Path) -> Path:
    resolved = path.resolve(strict=False)
    resolved_parent = parent.resolve(strict=True)
    if resolved != resolved_parent and resolved_parent not in resolved.parents:
        raise ValidationError(f"unsafe path outside {resolved_parent}: {resolved}")
    return resolved


def require_no_symlink(path: Path) -> None:
    current = path
    while current != current.parent:
        if current.is_symlink():
            raise ValidationError(f"refusing symlink path: {current}")
        if current == ROOT:
            return
        current = current.parent


def format_list(items: Iterable[str], limit: int = 8) -> str:
    values = sorted(items)
    if len(values) <= limit:
        return ", ".join(values)
    return ", ".join(values[:limit]) + f", and {len(values) - limit} more"
