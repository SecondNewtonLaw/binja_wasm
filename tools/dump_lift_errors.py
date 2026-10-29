"""Dump every lifting problem in the open WASM view.

Run inside Binary Ninja: Tools > Run Script (or paste in the Python console, where `bv` exists).
Writes two files next to OUT_PREFIX:
  *_errors.tsv   kind, function, address, opcode, disassembly
  *_summary.txt  counts per kind and per opcode, plus a few sample addresses each
"""
import collections
import os
import threading
import time

from binaryninja import LowLevelILOperation as Op

OUT_PREFIX = os.path.join(os.path.expanduser("~"), "wasm_lift")
MAX_SAMPLES = 5
LIMIT = 2000  # functions to scan

BAD = {
    Op.LLIL_UNIMPL: "unimpl",
    Op.LLIL_UNIMPL_MEM: "unimpl_mem",
    Op.LLIL_UNDEF: "undef",
}


def opcode(text):
    return text.split(" ", 1)[0] if text else "?"


def main(bv):
    rows = []
    counts = collections.Counter()
    by_opcode = collections.defaultdict(collections.Counter)
    samples = collections.defaultdict(list)

    def record(kind, func, addr, text):
        op = opcode(text)
        rows.append((kind, func.name, addr, op, text))
        counts[kind] += 1
        by_opcode[kind][op] += 1
        if len(samples[(kind, op)]) < MAX_SAMPLES:
            samples[(kind, op)].append(addr)

    funcs = list(bv.functions)[:LIMIT]
    print(f"scanning {len(funcs)} functions")
    t0 = time.time()
    for n, func in enumerate(funcs):
        if n % 200 == 0:
            print(f"  {n}/{len(funcs)}  {time.time() - t0:.0f}s", flush=True)
        try:
            llil = func.llil
        except Exception as e:  # noqa: BLE001
            rows.append(("llil_exception", func.name, func.start, "?", str(e)))
            counts["llil_exception"] += 1
            continue
        if llil is None:
            record("no_llil", func, func.start, "")
            continue
        for block in llil:
            for ins in block:
                kind = BAD.get(ins.operation)
                if kind is None and ins.operation == Op.LLIL_JUMP and ins.dest.operation not in (
                    Op.LLIL_CONST, Op.LLIL_CONST_PTR):
                    kind = "indirect_jump"
                if kind:
                    record(kind, func, ins.address, bv.get_disassembly(ins.address))
        for ref in getattr(func, "unresolved_indirect_branches", []):
            addr = getattr(ref, "addr", None)
            if addr is None:
                addr = ref[1]
            record("unresolved_branch", func, addr, bv.get_disassembly(addr))

    # Code bytes that no function covers (decode gaps, past-the-end instructions).
    code = [s for s in bv.segments if s.executable and s.start >= 0xC0000000 and s.start < 0xF0000000]
    for seg in (code if len(funcs) == len(bv.functions) else []):
        addr = seg.start
        gaps = 0
        while addr < seg.end and gaps < 5000:
            if bv.get_functions_containing(addr):
                addr = max(f.highest_address for f in bv.get_functions_containing(addr)) + 1
                continue
            nxt = bv.get_next_function_start_after(addr)
            if nxt == 0 or nxt > seg.end:
                nxt = seg.end
            if nxt > addr:
                rows.append(("uncovered", "", addr, "?", f"{nxt - addr} bytes"))
                counts["uncovered"] += 1
                gaps += 1
            addr = nxt

    with open(OUT_PREFIX + "_errors.tsv", "w", encoding="utf-8") as f:
        f.write("kind\tfunction\taddress\topcode\ttext\n")
        for kind, name, addr, op, text in rows:
            f.write(f"{kind}\t{name}\t0x{addr:x}\t{op}\t{text}\n")

    with open(OUT_PREFIX + "_summary.txt", "w", encoding="utf-8") as f:
        f.write(f"functions: {len(funcs)}\n")
        for kind, c in counts.most_common():
            f.write(f"\n{kind}: {c}\n")
            for op, k in by_opcode[kind].most_common(40):
                s = ", ".join(f"0x{a:x}" for a in samples[(kind, op)])
                f.write(f"  {op:<28}{k:>8}  e.g. {s}\n")

    print(f"done: {sum(counts.values())} problems")
    print(OUT_PREFIX + "_errors.tsv")
    print(OUT_PREFIX + "_summary.txt")


threading.Thread(target=main, args=(bv,), daemon=True).start()  # noqa: F821  (`bv` comes from BN)
