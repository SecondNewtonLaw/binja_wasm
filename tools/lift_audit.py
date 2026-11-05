#!/usr/bin/env python3
"""Aggregate wasm_lift_probe JSON into a lift-quality report.

Usage:
    python tools/lift_audit.py path/to/wasm_lift_probe path/to/modules [--out report.md]
    python tools/lift_audit.py build/wasm_lift_probe module.wasm

The probe is the headless C++ tool built by BINJA_WASM_BUILD_TESTS.  It uses the
same src/bn/lifter.cpp as the plugin, so this report measures the real lift
without needing Binary Ninja.
"""
from __future__ import annotations

import argparse
import collections
import json
import pathlib
import subprocess
import sys


def find_modules(path: pathlib.Path) -> list[pathlib.Path]:
    if path.is_file():
        return [path]
    return sorted(path.rglob("*.wasm"))


def run_probe(probe: pathlib.Path, module: pathlib.Path) -> dict:
    proc = subprocess.run(
        [str(probe), str(module), "--json"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.returncode != 0:
        return {"error": proc.stderr.strip() or f"exit {proc.returncode}"}
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        return {"error": f"bad JSON from probe: {exc}"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("probe", type=pathlib.Path)
    ap.add_argument("input", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=None)
    args = ap.parse_args()

    modules = find_modules(args.input)
    if not modules:
        print(f"no .wasm files under {args.input}", file=sys.stderr)
        return 2

    totals = collections.Counter()
    unimplemented_ops = collections.Counter()
    intrinsics = collections.Counter()
    rows = []
    failures = []

    for module in modules:
        report = run_probe(args.probe, module)
        if "error" in report:
            failures.append((module, report["error"]))
            rows.append((module, None))
            continue
        m = collections.Counter()
        for fn in report.get("reports", []):
            for key in (
                "wasm_instrs",
                "il_instrs",
                "unimplemented",
                "nops",
                "intrinsics",
                "loads",
                "stores",
                "calls",
                "spills",
                "errors",
                "native_int_arith",
                "native_float",
                "native_compare",
                "unknown_sets",
                "unresolved_tables",
            ):
                m[key] += int(fn.get(key, 0))
            m["functions"] += 1
            unimplemented_ops.update(fn.get("unimplemented_ops", {}))
            intrinsics.update(fn.get("intrinsic_counts", {}))
        m["failed"] = int(report.get("failed", 0))
        totals.update(m)
        rows.append((module, m))

    lines = ["# WASM lift audit", ""]
    lines.append(f"Modules: {len(modules)}  ")
    lines.append(f"Functions: {totals['functions']}  ")
    lines.append(f"WASM instructions: {totals['wasm_instrs']}  ")
    lines.append(f"IL instructions: {totals['il_instrs']}  ")
    wasm = totals["wasm_instrs"]
    lines.append(
        f"IL/WASM ratio: {totals['il_instrs'] / wasm:.2f}" if wasm else "IL/WASM ratio: n/a"
    )
    lines.append(f"Unimplemented: {totals['unimplemented']}  ")
    lines.append(f"Label/report errors: {totals['errors']}  ")
    lines.append(f"Spills: {totals['spills']}  ")
    lines.append(f"Unknown SP sets: {totals['unknown_sets']}  ")
    lines.append(f"Unresolved br_table labels: {totals['unresolved_tables']}  ")
    lines.append(f"Failed functions: {totals['failed']}  ")
    lines.append("")
    lines.append("## Per module")
    lines.append("")
    lines.append("| module | funcs | wasm insn | IL insn | unimpl | spills | intrinsics | errors |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for module, m in rows:
        if m is None:
            lines.append(f"| `{module.name}` | - | - | - | - | - | - | probe failed |")
            continue
        lines.append(
            f"| `{module.name}` | {m['functions']} | {m['wasm_instrs']} | {m['il_instrs']} | "
            f"{m['unimplemented']} | {m['spills']} | {m['intrinsics']} | {m['errors']} |"
        )
    lines.append("")

    if unimplemented_ops:
        lines += ["## Unimplemented opcodes", "", "| opcode | count |", "| --- | ---: |"]
        for op, count in unimplemented_ops.most_common(60):
            lines.append(f"| `{op}` | {count} |")
        lines.append("")

    if intrinsics:
        lines += ["## Intrinsics", "", "| intrinsic | count |", "| --- | ---: |"]
        for name, count in intrinsics.most_common(60):
            lines.append(f"| `{name}` | {count} |")
        lines.append("")

    if failures:
        lines += ["## Probe failures", ""]
        for module, err in failures:
            lines.append(f"- `{module}`: {err}")
        lines.append("")

    text = "\n".join(lines)
    if args.out:
        args.out.write_text(text, encoding="utf-8")
        print(f"wrote {args.out}")
    else:
        print(text)

    hard_fail = totals["unimplemented"] or totals["errors"] or totals["failed"] or failures
    return 1 if hard_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
