"""Binary Ninja check for tests/fixtures/lift_issues.wasm.

Open the module with the updated plugin, then run this in Binary Ninja:
  Tools > Run Script...
The report is printed and written to %TEMP%/wasm_lift_bn_report.txt.

It checks the specific fixes covered by the headless harness and prints the
LLIL/HLIL of each targeted function so the remaining readability can be judged.
"""
import os
import sys

import binaryninja as bn
from binaryninja import LowLevelILOperation as L

MAX_LINES = 700
TARGETS = [
    "empty_then",
    "brif_else",
    "uninit_local",
    "g0_set_const",
    "g0_via_local",
    "g1_bump",
    "g2_bump",
    "multi3",
    "call_multi",
    "many_args",
    "deep_stack",
    "sel",
    "apply_indirect",
    "apply_ref",
    "float32_const",
    "float64_const",
    "wrap64",
    "br_table",
    "block_param",
    "multi_block",
    "v128_id",
    "v128_select",
    "simd_spill",
    "simd_scalar",
    "clz",
]

BAD = {
    L.LLIL_UNIMPL: "unimpl",
    L.LLIL_UNIMPL_MEM: "unimpl_mem",
    L.LLIL_UNDEF: "undef",
}

out = []


def p(text=""):
    text = str(text)
    out.append(text)
    print(text)


def main(bv):
    if bv is None:
        p("no BinaryView: open lift_issues.wasm first")
        return

    p(f"file: {bv.file.filename}")
    p(f"view: {bv.view_type}  arch: {bv.arch.name}")
    regs = set(bv.arch.regs.keys())
    has_new_banks = any(r.startswith("gv") for r in regs) and any(r.startswith("tv") for r in regs)
    p(f"updated plugin marker (gv/tv registers present): {has_new_banks}")
    if not has_new_banks:
        p("WARNING: this looks like the old plugin build; close BN and replace the")
        p("manager copy at plugins/binja_wasm/bin/6.0/binja_wasm.dll with the new DLL.")

    problems = []
    funcs = list(bv.functions)
    p(f"functions: {len(funcs)}")
    subs = [f for f in funcs if f.name.startswith("sub_")]
    p(f"BN-produced sub_* functions: {len(subs)}")
    for f in subs[:40]:
        p(f"  {f.name} @ 0x{f.start:x}")
    for func in funcs:
        try:
            llil = func.llil
        except Exception as exc:  # noqa: BLE001
            problems.append((func.name, func.start, "llil_exception", str(exc)))
            continue
        if llil is None:
            problems.append((func.name, func.start, "no_llil", ""))
            continue
        for block in llil.basic_blocks:
            for ins in block:
                kind = BAD.get(ins.operation)
                if kind is None and ins.operation == L.LLIL_JUMP and ins.dest.operation not in (
                    L.LLIL_CONST,
                    L.LLIL_CONST_PTR,
                ):
                    kind = "indirect_jump"
                if kind:
                    problems.append((func.name, ins.address, kind, str(ins)))
        for ref in getattr(func, "unresolved_indirect_branches", []):
            addr = getattr(ref, "addr", None)
            if addr is None and isinstance(ref, (tuple, list)):
                addr = ref[1]
            problems.append((func.name, addr or func.start, "unresolved_branch", ""))

    p(f"problems: {len(problems)}")
    for name, addr, kind, text in problems[:300]:
        p(f"  {kind:<18} {name:<20} 0x{addr:x} {text}")

    for name in TARGETS:
        matches = bv.get_functions_by_name(name)
        if not matches:
            p(f"=== {name}: MISSING ===")
            continue
        func = matches[0]
        p(f"=== {name} @ 0x{func.start:x} ===")
        llil = func.llil
        if llil is None:
            p("  <no LLIL>")
            continue
        lines = []
        for block in llil.basic_blocks:
            for ins in block:
                lines.append(str(ins))
        p("\n".join(lines[:MAX_LINES]))
        if len(lines) > MAX_LINES:
            p(f"  ... {len(lines) - MAX_LINES} LLIL lines omitted ...")
        try:
            hlil = func.hlil
            if hlil is not None:
                p("  --- HLIL ---")
                hlines = str(hlil).splitlines()
                p("\n".join(hlines[:MAX_LINES]))
        except Exception as exc:  # noqa: BLE001
            p(f"  HLIL failed: {exc}")

        text = "\n".join(lines)
        checks = []
        if name == "uninit_local":
            checks.append(("zero-init local", "ld0 = 0" in text))
        if name == "many_args":
            checks.append(("a9 read", "a9" in text))
        if name == "multi3":
            checks.append(("r2 written", "r2" in text))
        if name in ("g0_set_const", "g0_via_local"):
            checks.append(("g0 write not nop", "g0 = " in text or "g0 =" in text))
        if name == "g1_bump":
            checks.append(("g1 accessed as 4 bytes", "g1.d" in text))
        if name == "g2_bump":
            checks.append(("g2 is 8 bytes", bv.arch.regs["g2"].size == 8))
        if name == "sel":
            checks.append(("no self-move", "s0 = s0" not in text))
        if name == "clz":
            checks.append(("direct intrinsic", "__builtin_i32_clz" in text and "t8" not in text))
        if name == "float32_const":
            checks.append(("float displayed", "1.5" in text))
        if name == "apply_indirect":
            checks.append(("table_target", "__builtin_table_target" in text))
        if name == "v128_id":
            checks.append(("64-bit split/join", "0x40" in text or "64" in text))
        if name == "simd_spill":
            checks.append(("16-byte tv0 result", "tv0" in text))
        if name == "simd_scalar":
            checks.append(("scalar temp result", "temp8" in text and "__builtin_i32x4_all_true" in text))
        if name == "empty_then":
            # The true edge must land after the else body (the local stays 0).
            checks.append(("then path skips else", text.count("sd0 = 7") <= 1))
        if name == "brif_else":
            checks.append(("br_if false edge skips else", text.count("sd0 = 2") <= 1))
        for label, ok in checks:
            p(f"  CHECK {'PASS' if ok else 'FAIL'}: {label}")

    path = os.path.join(os.environ.get("TEMP", "."), "wasm_lift_bn_report.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    p(f"\nreport: {path}")


if "bv" not in globals() or globals()["bv"] is None:
    print("Open tests/fixtures/lift_issues.wasm in Binary Ninja, then run this script.")
else:
    main(globals()["bv"])
