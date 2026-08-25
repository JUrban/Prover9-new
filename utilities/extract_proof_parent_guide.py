#!/usr/bin/env python3
"""Extract an auxiliary proof-parent guide from Prover9 proof output.

The emitted file is native Prover9 input.  It contains only an auxiliary
``formulas(proof_parent_guide)`` list; loading it never adds the proof clauses
to the logical theory.
"""

from __future__ import annotations

import argparse
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


class GuideError(RuntimeError):
    pass


@dataclass
class ProofRecord:
    source_id: int
    body: str
    justification: str
    rule: str
    parents: tuple[int, ...]
    rewrite_parents: tuple[int, ...]


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


def parse_primary(justification: str) -> tuple[str, tuple[int, ...]]:
    for name in ("para", "hyper", "resolve", "back_rewrite", "copy"):
        args = extract_call(justification, name)
        if args is None:
            continue
        fields = split_top_level(args)
        if name == "para":
            parents = tuple(
                parent for parent in (leading_id(field) for field in fields)
                if parent is not None
            )
            if len(parents) != 2:
                raise GuideError(
                    f"paramodulation does not have two parents: {justification}"
                )
        elif name in ("hyper", "resolve"):
            # Hyper/resolve output alternates source clause IDs with literal
            # designators.  Only fields that are plain decimal integers are
            # proof parents.
            parents = tuple(
                int(field) for field in fields if re.fullmatch(r"\d+", field)
            )
            if len(parents) < 2:
                raise GuideError(
                    f"{name} does not have at least two parents: {justification}"
                )
        else:
            parent = leading_id(fields[0]) if fields else None
            if parent is None:
                raise GuideError(f"{name} has no parent: {justification}")
            parents = (parent,)
        return name, parents

    for name in ("assumption", "goal", "deny"):
        if re.search(r"(?:^|,)\s*" + re.escape(name) + r"(?:\]|,|$)",
                     justification):
            return name, ()
    return "other", ()


def parse_rewrites(justification: str) -> tuple[int, ...]:
    args = extract_call(justification, "rewrite")
    if args is None:
        return ()
    args = args.strip()
    if not (args.startswith("[") and args.endswith("]")):
        raise GuideError(f"malformed rewrite list: {justification}")
    parents: list[int] = []
    for field in split_top_level(args[1:-1]):
        if not field:
            continue
        parent = leading_id(field)
        if parent is None:
            raise GuideError(f"malformed rewrite parent {field!r}")
        parents.append(parent)
    return tuple(parents)


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
    rule, parents = parse_primary(justification)
    rewrites = parse_rewrites(justification)
    return ProofRecord(source_id, body, justification, rule, parents, rewrites)


def proof_statements(lines: Iterable[str]) -> Iterable[str]:
    in_proof = False
    current: list[str] = []
    found = False
    for raw_line in lines:
        line = raw_line.rstrip("\r\n")
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
    with open_input(path) as stream:
        records = [parse_record(statement) for statement in proof_statements(stream)]
    if not records:
        raise GuideError("the PROOF section contains no clauses")

    indexes: dict[int, int] = {}
    for index, record in enumerate(records):
        if record.source_id in indexes:
            raise GuideError(f"duplicate proof clause ID {record.source_id}")
        indexes[record.source_id] = index
    for index, record in enumerate(records):
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
