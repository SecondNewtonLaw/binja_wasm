# Headless lift harness

This directory validates the native `WASM -> BNIL` lifter without Binary Ninja.
It compiles the real `src/bn/lifter.cpp` against a small partial IL
(`tests/support/partial_il.hpp`) instead of the BN API, then checks the resulting
IL structurally and semantically.

```
 tests/fixtures/sample.c ──clang──> sample.wasm
                                        │
 ModuleBuilder ──> wasm::parseModule ──> wasm::analyze
                                        │
                        bnwasm::lift (the shipping implementation)
                                        │
                              ┌─────────┴─────────┐
                              ▼                   ▼
                      partial IL tree       partial IL interpreter
                              │                   │
                        printer/stats      compare results/memory/globals
                              │                   │
                              └─────────┬─────────┘
                                        ▼
                           independent WASM interpreter (oracle)
```

## What is tested

| Test | What it proves |
| --- | --- |
| `numeric_i32`, `numeric_i64`, `numeric_f32`, `numeric_f64` | Every integer/float/comparison opcode produces the same result as the WASM oracle over edge-value and cross-product inputs. |
| `conversions`, `trunc_sat` | Signed/unsigned conversions, reinterpretations, sign extensions, and saturating truncations. |
| `memory_loads`, `memory_stores`, `bulk_memory`, `memory_size_grow` | All load/store widths, sign/zero extension, memarg offsets, `memory.copy/fill/init`, growth, and final memory contents. |
| `locals_tee`, `zero_init_locals`, `globals` | Local/global reads/writes, `local.tee`, WASM's zero-initialised locals, globals beyond the register bank, and the `global 0` stack-pointer heuristic. |
| `if_else`, `if_empty_then`, `if_else_br_if`, `if_else_br_if_last`, `block_br`, `br_if`, `loop`, `br_table`, `select` | Structured control flow, including the tricky empty-then and conditional-branch-at-the-else-boundary cases, branch arguments, and value selection. |
| `call`, `call_indirect`, `call_ref`, `recursion`, `return_call`, `multi_value`, `many_args` | Direct/indirect/ref calls, table dispatch, recursion, tail calls, multi-value returns, and argument passing. |
| `v128_transport`, `v128_select`, `v128_local` | 128-bit values across parameters, results, locals, and `select`, including the 64-bit join/split shifts. |
| `deep_stack`, `simd_spill_shape` | Operand slots beyond `kNStack` spill through `sp` and still compute correctly; SIMD results take the same spill path (structure checked because the oracle does not model SIMD intrinsics). |
| `readability` | Core arithmetic is native IL, not an intrinsic; one-output intrinsics land directly in the stack slot; floats print as floats; v128 joins/splits use 64-bit shifts. |
| `coverage` | 531 generated functions covering every decodable plain/FC/FD/FE opcode family; fails on `unimplemented` IL or unmarked labels. |
| `golden` | A stable, reviewable IL snapshot in `tests/golden/lift_snapshots.txt`. |
| `real_sample`, `complex_sample` | Clang-produced modules (`tests/fixtures/sample.wasm`, `complex.wasm`) run through both engines: recursion, br_table, loops, floats, memory, shadow stack, popcount/clz, function pointers via `call_indirect`, and v128 transport. Enabled by `WASM_TEST_MODULE` / `WASM_COMPLEX_MODULE`. |
| `issues_sample` | `tests/fixtures/lift_issues.wasm`, a hand-written regression module for every bug fixed by the harness (empty then, br_if at the else boundary, zero locals, global 0 as an ordinary counter, global widths, multi-value, 10 arguments, spills, select, call_indirect/call_ref, float constants, wrap_i64, br_table, bulk memory, block params, v128, SIMD spill, clz/popcnt). Enabled by `WASM_ISSUES_MODULE`. |

## Files

- `support/partial_il.hpp` — BN-compatible `LowLevelILFunction` subset, expression tree, labels, printer, and `analyzeIl` stats.
- `support/lift_runner.hpp` — drives `bnwasm::lift` one instruction at a time, matching `WasmArchitecture::GetInstructionLowLevelIL`.
- `support/wasm_builder.hpp` — minimal WASM binary encoder used by the tests.
- `support/wasm_interp.hpp` — independent WASM interpreter (the oracle); unsupported features raise `Unsupported` so tests skip rather than compare against wrong semantics.
- `support/il_interp.hpp` — interpreter for the partial IL, including register sub-width/zero-extension semantics, calls, labels, memory, and the intrinsics needed by the tests.
- `lift_coverage.hpp` — opcode coverage matrix generator.
- `lift_tests.cpp` — the test cases and main.
- `golden/lift_snapshots.txt` — expected IL for representative functions.
- `../tools/lift_probe.cpp` — CLI report for a single module, usable on a real corpus.
- `../tools/lift_audit.py` — aggregates probe JSON into a Markdown report.

## Running

```
cmake --preset release -DBINJA_WASM_BUILD_PLUGIN=OFF
cmake --build --preset release
ctest --test-dir cmake-build-release --output-on-failure
```

Run a subset directly:

```
./cmake-build-release/wasm_lift_tests numeric_i32
./cmake-build-release/wasm_lift_tests coverage
```

Probe:

```
./cmake-build-release/wasm_lift_probe tests/fixtures/sample.wasm
./cmake-build-release/wasm_lift_probe tests/fixtures/sample.wasm --list
./cmake-build-release/wasm_lift_probe tests/fixtures/sample.wasm --dump 0x1a3
./cmake-build-release/wasm_lift_probe tests/fixtures/sample.wasm --json --check
python tools/lift_audit.py ./cmake-build-release/wasm_lift_probe corpus/ --out report.md
```

## Regression sample for Binary Ninja

`tests/fixtures/lift_issues.wat` is the targeted sample for the fixes above.
Rebuild it with WABT:

```
wat2wasm --enable-all --debug-names tests/fixtures/lift_issues.wat -o tests/fixtures/lift_issues.wasm
```

Open `tests/fixtures/lift_issues.wasm` in Binary Ninja with the updated plugin,
then run `tools/bn_check_lift.py` from `Tools > Run Script`. The script checks
that the new register banks (`gv*`/`tv*`) are present, scans every function for
`LLIL_UNIMPL`/`LLIL_UNDEF`, and prints the LLIL/HLIL of each target function
with per-case checks (zero local, `a9`, tuple return, `g0` write, 4-byte `g1`,
no `select` self-move, direct `clz`, float display, table target, `uint128_t`
result, recovered `switch`, SIMD `tv0`).

## Updating the golden snapshot

Review the diff, then:

```
WASM_UPDATE_GOLDEN=1 ./cmake-build-release/wasm_lift_tests golden
```

## Adding a test

Most opcode tests are a `ModuleBuilder` + one `Code` body. The `Fixture::check`
helper runs the WASM oracle and the lifted IL with identical inputs and compares
results, memory, and globals. For a new opcode, add it to the appropriate driver
in `lift_tests.cpp`; for a new opcode family, add it to `lift_coverage.hpp` so
the coverage matrix grows with it.

## References

- The [WebAssembly core specification](https://www.w3.org/TR/wasm-core-2/) is the
  semantic authority for the oracle.
- [wabt/wasm2c](https://github.com/WebAssembly/wabt/tree/main/wasm2c) is the
  reference WASM-to-C lowering used when deciding how to model memory, the
  shadow stack, and intrinsics.
- The original Python [binjawa](https://github.com/ivision-research/binjawa)
  lifter established the register/stack-slot model that this native lifter
  ports; the headless harness is what makes that model testable.

## Known boundaries

- SIMD, atomics, and bulk-memory are lifted to `__builtin_*` intrinsics; the
  harness checks their stack effects and coverage, not the intrinsic semantics.
- Exceptions and legacy `try`/`delegate` are covered structurally only.
- The model supports 16 arguments and 8 results; larger signatures emit
  `unimplemented` deliberately.
- Locals beyond 128 and globals beyond the register banks use `__builtin_*`
  accessors; the semantic oracle does not execute the local variants.
- `call_indirect` semantics are modelled for table 0 (the common case); other
  tables are lifted structurally.
- `br_table` with resolved targets and no per-edge moves or stack fixups is
  lifted to a selector-keyed `JumpTo`, which BN recovers as a real
  `switch (selector) { case 0 ... }`.  Tables with different moves/SP
  adjustments per label still use a compare chain.  BN may list the
  selector-keyed target list in `unresolved_indirect_branches`; the actual case
  targets are registered through `SetAutoIndirectBranches`, so the check treats
  a `LLIL_JUMP_TO` at that address as resolved.
- The `sp` spill pointer is an external scratch register. The IL interpreter
  initialises it; Binary Ninja's decompiler treats it as an ordinary register
  and prints spill-heavy HLIL through `entry_sp` memory accesses.
- Binary Ninja's function type still has one return slot, but a return slot can
  now be a packed tuple: multi-value functions get a structure return type and a
  return location listing `r0..r7`.  HLIL reconstructs the tuple:
  `return struct { .r0 = ..., .r1 = ..., .r2 = ... }`.
- v128 is modelled as one 16-byte `uint128_t` rather than two unrelated u64
  halves.  The return location maps `r{k}` at offset 0 and `rh{k}` at offset 8,
  so HLIL builds a complete 128-bit result.
- Float returns use a same-size `fconvert` identity in the epilogue.  Without
  it, Binary Ninja folds a float constant copied through an integer register
  into an integer constant and HLIL shows the raw bits (`0x3fc00000`).  With
  the conversion, HLIL prints `fconvert.s(1.5f)` / `fconvert.d(3.14159...)`.
