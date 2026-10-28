"""Small ELF/disassembly helpers shared by read-only policy reports."""

from __future__ import annotations

import re
import sys
from collections import deque
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import run, stm32_tool

INDIRECT_CALL_RE = re.compile(
    r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2,8}\s+)+"
    r"(?:blx(?:eq|ne|cs|cc|mi|pl|hi|ls|ge|lt|gt|le)?\s+(?:r(?:[0-9]|1[0-5])|sl|fp|ip|lr)|"
    r"bx(?:eq|ne|cs|cc|mi|pl|hi|ls|ge|lt|gt|le)?\s+(?:r(?:[0-9]|1[0-2])|sl|fp|ip))\s*$"
)


def normalize_symbol(name: str) -> str:
    without_offset = name.split("+", 1)[0]
    return re.split(r"\.(?:constprop|isra|part|cold)(?:\.[0-9]+)?$",
                    without_offset, maxsplit=1)[0]


def is_indirect_call_instruction(line: str) -> bool:
    return INDIRECT_CALL_RE.match(line) is not None


def disassembly_blocks(output: str) -> dict[str, list[str]]:
    """Preserve duplicate IAR outlined helpers and inline tail-call annotations.

    The ST PD archive contains several unrelated `.text_21` helpers and
    __iar_annotation labels in the middle of functions. Merging helpers by
    label name creates false recursion; treating annotations as functions
    disconnects real callback paths.
    """
    headers = re.compile(r"^([0-9a-fA-F]+)\s+<([^>]+)>:$")
    blocks: dict[str, list[str]] = {}
    addresses: dict[int, str] = {}
    current = owner = None
    for line in output.splitlines():
        match = headers.match(line)
        if match:
            address, label = int(match[1], 16), match[2]
            if label.startswith("__iar_annotation") and current:
                addresses[address] = current
                continue
            name = normalize_symbol(label)
            if name.startswith(".text"):
                name += "@" + (owner or "anonymous")
            else:
                owner = name
            current = name
            addresses[address] = name
            blocks.setdefault(name, [])
        elif current:
            blocks[current].append(line)
    # Resolve branch labels through their address, not a non-unique name.
    from bisect import bisect_right
    ordered = sorted(addresses)
    branch = re.compile(r"(\b(?:blx?|b(?:eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?)(?:\.[nw])?\s+)([0-9a-fA-F]+)\s+<[^>]+>")
    for lines in blocks.values():
        for i, line in enumerate(lines):
            match = branch.search(line)
            if match:
                index = bisect_right(ordered, int(match[2], 16)) - 1
                if index >= 0:
                    target = addresses[ordered[index]]
                    lines[i] = line[:match.start()] + match[1] + match[2] + " <" + target + ">"
    return blocks


def elf_blocks(elf: Path) -> dict[str, list[str]]:
    return disassembly_blocks(run([stm32_tool("objdump"), "-d", elf]).stdout)


def graph_from_blocks(blocks: dict[str, list[str]]) -> dict[str, set[str]]:
    graph = {name: set() for name in blocks}
    instruction = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2,8}\s+)+([a-z][a-z0-9.]*)\s+.*<([^>]+)>")
    for current, lines in blocks.items():
        for line in lines:
            match = instruction.match(line)
            if not match:
                continue
            mnemonic, target = match.groups()
            # Cross-function conditional tail branches also carry reachability.
            if mnemonic in {"bl", "blx"} or (mnemonic.startswith("b") and mnemonic not in {"bic", "bics", "bfi", "bfc", "bx"} and target != current):
                graph[current].add(target)
        if current.startswith("__") and current.endswith("_veneer"):
            graph[current].add(current[2:-len("_veneer")])
    return graph


def call_graph(elf: Path) -> dict[str, set[str]]:
    return graph_from_blocks(elf_blocks(elf))


def indirect_callers(elf: Path) -> set[str]:
    return {name for name, lines in elf_blocks(elf).items()
            if any(is_indirect_call_instruction(line) for line in lines)}


def shortest_path(graph: dict[str, set[str]], root: str, target: str) -> list[str] | None:
    queue: deque[tuple[str, list[str]]] = deque([(root, [root])])
    seen = {root}
    while queue:
        node, path = queue.popleft()
        if node == target:
            return path
        for child in graph.get(node, ()):
            if child not in seen:
                seen.add(child)
                queue.append((child, [*path, child]))
    return None


def reachable(graph: dict[str, set[str]], root: str) -> set[str]:
    found: set[str] = set()
    pending = [root]
    while pending:
        node = pending.pop()
        if node in found:
            continue
        found.add(node)
        pending.extend(graph.get(node, ()))
    return found
