"""Headless: load the wasm, analyse one function, print its LLIL and HLIL.

usage: python dump_func.py <wasm> <hex address>
"""
import sys

sys.path.insert(0, r"C:\Users\Dottik\AppData\Local\Programs\Vector35\BinaryNinja\python")
import binaryninja as bn

path, addr = sys.argv[1], int(sys.argv[2], 16)
bv = bn.load(path, options={"analysis.limits.maxFunctionSize": 0})
print("view:", bv.view_type)
f = bv.create_user_function(addr) or bv.get_function_at(addr)
bv.update_analysis_and_wait()
print("func:", f, "blocks:", len(list(f.llil.basic_blocks)))
for blk in f.llil.basic_blocks:
    print(f"-- block {blk.start}..{blk.end} -> {[e.target.start for e in blk.outgoing_edges]}")
    for i in blk:
        print(f"  {i.instr_index:4} {i.address:#x} {i}")
print("=== HLIL")
print(f.hlil)
