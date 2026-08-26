#!/usr/bin/env python3
"""Extract an auxiliary proof-parent guide from Prover9 proof output.

The emitted file is native Prover9 input.  It contains only an auxiliary
``formulas(proof_parent_guide)`` list; loading it never adds the proof clauses
to the logical theory.
"""

from __future__ import annotations

import argparse
import base64
import gzip
import io
import os
import re
import sys
from dataclasses import dataclass
from typing import Iterable, TextIO


PROOF_START = "============================== PROOF "
PROOF_END = "============================== end of proof"
CLAUSE_START = re.compile(r"^(\d+)\s+(.*)$")
GIVEN_CLAUSE = re.compile(r"^given\s+#\d+\s+.*?:\s+(\d+)\s")


class GuideError(RuntimeError):
    pass


RECIPE_VERSION = 1

RULE_CODES = {
    "assumption": 1,
    "goal": 2,
    "deny": 3,
    "copy": 4,
    "back_rewrite": 5,
    "para": 6,
    "hyper": 7,
    "resolve": 8,
}

SECONDARY_REWRITE = 1
SECONDARY_FLIP = 2


@dataclass(frozen=True)
class PositionedParent:
    source_id: int
    position: tuple[int, ...]


@dataclass(frozen=True)
class ResolutionClash:
    nucleus_literal: int
    satellite_id: int
    satellite_literal: int


@dataclass(frozen=True)
class ResolutionRecipe:
    nucleus_id: int
    clashes: tuple[ResolutionClash, ...]


@dataclass(frozen=True)
class RewriteStep:
    source_id: int
    target: int
    direction: int


@dataclass(frozen=True)
class FlipStep:
    literal: int


@dataclass
class ProofRecord:
    source_id: int
    body: str
    justification: str
    rule: str
    parents: tuple[int, ...]
    rewrite_parents: tuple[int, ...]
    primary_data: object | None
    secondary_steps: tuple[RewriteStep | FlipStep, ...]
    source_given: bool = False


def open_input(path: str) -> TextIO:
    if path == "-":
        return sys.stdin
    raw = open(path, "rb")
    magic = raw.read(2)
    raw.seek(0)
    if magic == b"\x1f\x8b" or path.endswith(".gz"):
        return io.TextIOWrapper(gzip.GzipFile(fileobj=raw), encoding="utf-8")
    return io.TextIOWrapper(raw, encoding="utf-8")


def extract_call(text: str, name: str) -> str | None:
    marker = name + "("
    match = re.search(r"(?<![A-Za-z0-9_])" + re.escape(marker), text)
    if match is None:
        return None
    start = match.start()
    at = start + len(marker)
    depth = 1
    quote = False
    escaped = False
    for index in range(at, len(text)):
        char = text[index]
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[at:index]
    raise GuideError(f"unterminated {name}(...) justification: {text}")


def split_top_level(text: str) -> list[str]:
    fields: list[str] = []
    start = 0
    depth = 0
    quote = False
    escaped = False
    for index, char in enumerate(text):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        elif char == "," and depth == 0:
            fields.append(text[start:index].strip())
            start = index + 1
    fields.append(text[start:].strip())
    return fields


def leading_id(field: str) -> int | None:
    match = re.match(r"^(\d+)(?:\s*$|\s*\()", field)
    return int(match.group(1)) if match else None


def parse_literal_designator(field: str) -> int:
    """Translate Prover9's a/b/... and a(flip) literal notation."""
    field = field.strip()
    match = re.fullmatch(r"([A-Za-z]+)(?:\(flip\))?", field)
    if match is None:
        raise GuideError(f"malformed literal designator {field!r}")
    letters = match.group(1).lower()
    value = 0
    for letter in letters:
        if not "a" <= letter <= "z":
            raise GuideError(f"malformed literal designator {field!r}")
        value = value * 26 + ord(letter) - ord("a") + 1
    return -value if field.endswith("(flip)") else value


def parse_positioned_parent(field: str) -> PositionedParent:
    match = re.fullmatch(r"(\d+)\((.*)\)", field.strip())
    if match is None:
        raise GuideError(f"malformed positioned proof parent {field!r}")
    position_fields = split_top_level(match.group(2))
    if not position_fields:
        raise GuideError(f"empty proof position in {field!r}")
    position = [parse_literal_designator(position_fields[0])]
    for item in position_fields[1:]:
        if not re.fullmatch(r"\d+", item):
            raise GuideError(f"malformed proof position component {item!r}")
        value = int(item)
        if value <= 0:
            raise GuideError(f"nonpositive proof position component {item!r}")
        position.append(value)
    return PositionedParent(int(match.group(1)), tuple(position))


def parse_resolution(name: str, args: str) -> ResolutionRecipe:
    fields = split_top_level(args)
    if len(fields) < 4 or (len(fields) - 1) % 3 != 0:
        raise GuideError(
            f"{name} must contain a nucleus followed by literal/"
            f"satellite/literal triples: {args}"
        )
    if not re.fullmatch(r"\d+", fields[0]):
        raise GuideError(f"{name} has a malformed nucleus ID: {fields[0]!r}")
    clashes: list[ResolutionClash] = []
    for at in range(1, len(fields), 3):
        if not re.fullmatch(r"\d+", fields[at + 1]):
            raise GuideError(
                f"{name} has a malformed satellite ID: {fields[at + 1]!r}"
            )
        clashes.append(ResolutionClash(
            parse_literal_designator(fields[at]),
            int(fields[at + 1]),
            parse_literal_designator(fields[at + 2]),
        ))
    return ResolutionRecipe(int(fields[0]), tuple(clashes))


def parse_primary(
    justification: str,
) -> tuple[str, tuple[int, ...], object | None]:
    fields = split_top_level(justification)
    if not fields:
        return "other", (), None
    primary = fields[0]

    for name in ("para", "hyper", "resolve", "back_rewrite", "copy", "deny"):
        args = extract_call(primary, name)
        if args is None:
            continue
        if name == "para":
            parent_fields = split_top_level(args)
            if len(parent_fields) != 2:
                raise GuideError(
                    f"paramodulation does not have two parents: {justification}"
                )
            positioned = tuple(
                parse_positioned_parent(field) for field in parent_fields
            )
            return name, tuple(parent.source_id for parent in positioned), positioned
        if name in ("hyper", "resolve"):
            resolution = parse_resolution(name, args)
            parents = (resolution.nucleus_id,) + tuple(
                clash.satellite_id for clash in resolution.clashes
            )
            return name, parents, resolution
        parent_fields = split_top_level(args)
        parent = leading_id(parent_fields[0]) if parent_fields else None
        if parent is None:
            raise GuideError(f"{name} has no parent: {justification}")
        return name, (parent,), parent

    for name in ("assumption", "goal"):
        if primary.strip() == name:
            return name, (), None
    return "other", (), None


def parse_secondary(justification: str) -> tuple[RewriteStep | FlipStep, ...]:
    fields = split_top_level(justification)
    steps: list[RewriteStep | FlipStep] = []
    for field in fields[1:]:
        rewrite_args = extract_call(field, "rewrite")
        if rewrite_args is not None:
            rewrite_args = rewrite_args.strip()
            if not (rewrite_args.startswith("[") and
                    rewrite_args.endswith("]")):
                raise GuideError(f"malformed rewrite list: {justification}")
            for item in split_top_level(rewrite_args[1:-1]):
                if not item:
                    continue
                match = re.fullmatch(
                    r"(\d+)(?:\((\d+)(?:,(R))?\))?", item.strip()
                )
                if match is None:
                    raise GuideError(f"malformed rewrite step {item!r}")
                target = int(match.group(2)) if match.group(2) else 0
                steps.append(RewriteStep(
                    int(match.group(1)), target,
                    2 if match.group(3) else 1,
                ))
            continue
        flip_args = extract_call(field, "flip")
        if flip_args is not None:
            steps.append(FlipStep(parse_literal_designator(flip_args)))
            continue
        raise GuideError(f"unsupported secondary justification {field!r}")
    return tuple(steps)


def parse_record(text: str) -> ProofRecord:
    match = CLAUSE_START.match(text.strip())
    if match is None:
        raise GuideError(f"malformed proof clause: {text[:160]}")
    source_id = int(match.group(1))
    rest = match.group(2)
    split = rest.rfind(".  [")
    if split < 0 or not rest.endswith("]."):
        raise GuideError(f"cannot separate clause and justification at {source_id}")
    body = rest[:split].strip()
    justification = rest[split + 4:-2].strip()
    rule, parents, primary_data = parse_primary(justification)
    secondary = parse_secondary(justification)
    rewrites = tuple(
        step.source_id for step in secondary if isinstance(step, RewriteStep)
    )
    return ProofRecord(
        source_id, body, justification, rule, parents, rewrites,
        primary_data, secondary,
    )


def proof_statements(
    lines: Iterable[str], given_ids: set[int] | None = None,
) -> Iterable[str]:
    in_proof = False
    current: list[str] = []
    found = False
    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
        if not in_proof and given_ids is not None:
            given_match = GIVEN_CLAUSE.match(line)
            if given_match is not None:
                given_ids.add(int(given_match.group(1)))
        if line.startswith(PROOF_START):
            if in_proof:
                raise GuideError("nested PROOF section")
            in_proof = True
            found = True
            continue
        if line.startswith(PROOF_END):
            if current:
                yield " ".join(current)
                current = []
            in_proof = False
            break
        if not in_proof or not line or line.startswith("%"):
            continue
        if CLAUSE_START.match(line):
            if current:
                yield " ".join(current)
            current = [line]
        elif current:
            current.append(line.strip())
    if current:
        yield " ".join(current)
    if not found:
        raise GuideError("no Prover9 PROOF section found")


def read_records(path: str) -> list[ProofRecord]:
    given_ids: set[int] = set()
    with open_input(path) as stream:
        records = [
            parse_record(statement)
            for statement in proof_statements(stream, given_ids)
        ]
    if not records:
        raise GuideError("the PROOF section contains no clauses")

    indexes: dict[int, int] = {}
    for index, record in enumerate(records):
        if record.source_id in indexes:
            raise GuideError(f"duplicate proof clause ID {record.source_id}")
        indexes[record.source_id] = index
    for index, record in enumerate(records):
        record.source_given = record.source_id in given_ids
        for parent in record.parents + record.rewrite_parents:
            parent_at = indexes.get(parent)
            if parent_at is None:
                raise GuideError(
                    f"proof clause {record.source_id} references missing parent {parent}"
                )
            if parent_at >= index:
                raise GuideError(
                    f"proof clause {record.source_id} has non-prior parent {parent}"
                )
    return records


def counts(records: list[ProofRecord]) -> dict[str, int]:
    result: dict[str, int] = {"nodes": len(records), "rewrite_refs": 0}
    for record in records:
        result[record.rule] = result.get(record.rule, 0) + 1
        result["rewrite_refs"] += len(record.rewrite_parents)
    return result


def format_summary(result: dict[str, int]) -> str:
    keys = (
        "nodes", "para", "hyper", "back_rewrite", "resolve", "copy",
        "assumption", "goal", "deny", "other", "rewrite_refs",
    )
    return " ".join(f"{key}={result.get(key, 0)}" for key in keys)


def strip_top_level_attributes(body: str) -> str:
    """Remove printed clause attributes; exact clause identity ignores them."""
    depth = 0
    quote = False
    escaped = False
    for index, char in enumerate(body):
        if quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = False
            continue
        if char == '"':
            quote = True
        elif char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        elif char == "#" and depth == 0:
            return body[:index].rstrip()
    return body


def append_uvarint(output: bytearray, value: int) -> None:
    if value < 0:
        raise GuideError(f"cannot encode negative unsigned recipe value {value}")
    while value >= 0x80:
        output.append((value & 0x7f) | 0x80)
        value >>= 7
    output.append(value)


def append_svarint(output: bytearray, value: int) -> None:
    append_uvarint(output, value * 2 if value >= 0 else (-value * 2) - 1)


def append_position(output: bytearray, position: tuple[int, ...]) -> None:
    append_uvarint(output, len(position))
    for value in position:
        append_svarint(output, value)


def encode_recipe(record: ProofRecord, dense_id: dict[int, int]) -> str:
    code = RULE_CODES.get(record.rule)
    if code is None:
        raise GuideError(
            f"proof clause {record.source_id} has unsupported primary rule "
            f"{record.rule!r}: {record.justification}"
        )
    encoded = bytearray((RECIPE_VERSION, code, 1 if record.source_given else 0))

    if record.rule in ("deny", "copy", "back_rewrite"):
        assert isinstance(record.primary_data, int)
        append_uvarint(encoded, dense_id[record.primary_data])
    elif record.rule == "para":
        assert isinstance(record.primary_data, tuple)
        if len(record.primary_data) != 2:
            raise GuideError("internal paramodulation recipe arity error")
        for parent in record.primary_data:
            assert isinstance(parent, PositionedParent)
            append_uvarint(encoded, dense_id[parent.source_id])
            append_position(encoded, parent.position)
    elif record.rule in ("hyper", "resolve"):
        resolution = record.primary_data
        assert isinstance(resolution, ResolutionRecipe)
        append_uvarint(encoded, dense_id[resolution.nucleus_id])
        append_uvarint(encoded, len(resolution.clashes))
        for clash in resolution.clashes:
            append_svarint(encoded, clash.nucleus_literal)
            append_uvarint(encoded, dense_id[clash.satellite_id])
            append_svarint(encoded, clash.satellite_literal)

    append_uvarint(encoded, len(record.secondary_steps))
    for step in record.secondary_steps:
        if isinstance(step, RewriteStep):
            append_uvarint(encoded, SECONDARY_REWRITE)
            append_uvarint(encoded, dense_id[step.source_id])
            append_uvarint(encoded, step.target)
            append_uvarint(encoded, step.direction)
        elif isinstance(step, FlipStep):
            append_uvarint(encoded, SECONDARY_FLIP)
            append_svarint(encoded, step.literal)
        else:
            raise GuideError(f"unknown secondary recipe step {step!r}")
    return base64.urlsafe_b64encode(encoded).decode("ascii").rstrip("=")


def write_guide(records: list[ProofRecord], output: TextIO, source: str) -> None:
    result = counts(records)
    dense_id = {
        record.source_id: index + 1 for index, record in enumerate(records)
    }
    output.write("% Proof-parent guide generated by extract_proof_parent_guide.py\n")
    output.write(f"% Source: {os.path.basename(source)}\n")
    output.write(f"% {format_summary(result)}\n\n")
    output.write("formulas(proof_parent_guide).\n")
    for index, record in enumerate(records):
        # Parenthesize before adding attributes.  Without this, an attribute
        # following a disjunction attaches to its final literal instead of
        # the whole formula and is not moved to the resulting Topform.
        output.write(f"  ({strip_top_level_attributes(record.body)})\n")
        output.write(f"    # proof_parent_node({index + 1})\n")
        output.write(
            f'    # proof_parent_recipe("{encode_recipe(record, dense_id)}")\n'
        )
        # Unary copy/back-rewrite steps need no partner search, and ordinary
        # binary resolution remains deliberately unguided in this first
        # implementation.  Emit only the two expensive guided rule families.
        if record.rule in ("para", "hyper"):
            for parent in record.parents:
                output.write(
                    f"    # proof_parent_{record.rule}({dense_id[parent]})\n"
                )
        if record.rewrite_parents:
            for parent in record.rewrite_parents:
                output.write(
                    f"    # proof_parent_rewrite({dense_id[parent]})\n"
                )
        output.write("    .\n")
    output.write("end_of_list.\n")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="extract an auxiliary parent guide from Prover9 output"
    )
    parser.add_argument("proof_output", help="Prover9 .out, .out.gz, or -")
    parser.add_argument("-o", "--output", help="output include file")
    parser.add_argument(
        "--summary-only", action="store_true",
        help="validate the proof and print counts without emitting a guide",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        records = read_records(args.proof_output)
        summary = format_summary(counts(records))
        if args.summary_only:
            print(summary)
        else:
            if args.output:
                with open(args.output, "w", encoding="utf-8") as output:
                    write_guide(records, output, args.proof_output)
            else:
                write_guide(records, sys.stdout, args.proof_output)
            print(summary, file=sys.stderr)
    except (GuideError, OSError, UnicodeError) as error:
        print(f"extract_proof_parent_guide: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
