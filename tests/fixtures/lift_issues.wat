;; Targeted regression sample for the native WASM -> BNIL lifter.
;;
;; Build with:
;;   wat2wasm --enable-all --debug-names lift_issues.wat -o lift_issues.wasm
;;
;; Every exported function corresponds to a lift-quality case:

(module
  (type $unary (func (param i32) (result i32)))
  (type $multi3 (func (param i32 i32) (result i32 i32 i32)))
  (type $pair (func (result i32 i32)))
  (type $block_unary (func (param i32) (result i32)))
  (type $ten (func (param i32 i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))

  (memory 1)
  (table 2 funcref)
  (global $g0 (mut i32) (i32.const 5))
  (global $g1 (mut i32) (i32.const 9))
  (global $g2 (mut i64) (i64.const 10000000000))

  (elem (i32.const 0) $add_one $double_it)
  (data (i32.const 0x20) "\01\02\03\04")

  ;; --- empty then body: the true path must skip the else body -------------
  (func $empty_then (param $cond i32) (result i32)
    (local $x i32)
    local.get $cond
    if
    else
      i32.const 7
      local.set $x
    end
    local.get $x)

  ;; --- br_if is the last then-body instruction: false edge skips else ------
  (func $brif_else (param $outer i32) (param $inner i32) (result i32)
    local.get $outer
    if (result i32)
      i32.const 1
      local.get $inner
      br_if 0
    else
      i32.const 2
    end)

  ;; --- declared local must be zero-initialised -----------------------------
  (func $uninit_local (result i32)
    (local $x i32)
    local.get $x)

  ;; --- global 0 used as an ordinary counter, not the shadow stack ----------
  (func $g0_set_const (result i32)
    i32.const 42
    global.set $g0
    global.get $g0)

  (func $g0_via_local (result i32)
    (local $t i32)
    i32.const 43
    local.set $t
    local.get $t
    global.set $g0
    global.get $g0)

  ;; --- parsed global widths: g1 is i32, g2 is i64 -------------------------
  (func $g1_bump (result i32)
    global.get $g1
    i32.const 1
    i32.add
    global.set $g1
    global.get $g1)

  (func $g2_bump (result i64)
    global.get $g2
    i64.const 1
    i64.add
    global.set $g2
    global.get $g2)

  ;; --- multi-value results and calls --------------------------------------
  (func $multi3 (type $multi3)
    local.get 0
    local.get 1
    i32.add
    local.get 0
    local.get 1
    i32.sub
    local.get 0
    local.get 1
    i32.mul)

  (func $call_multi (type $multi3)
    local.get 0
    local.get 1
    call $multi3)

  ;; --- ten arguments (old bank was 8) --------------------------------------
  (func $many_args (type $ten)
    local.get 0
    local.get 1
    local.get 2
    local.get 3
    local.get 4
    local.get 5
    local.get 6
    local.get 7
    local.get 8
    local.get 9
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add)

  ;; --- deep scalar operand stack forces spills over kNStack ----------------
  (func $deep_stack (result i32)
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.const 1
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add
    i32.add)

  ;; --- select must not emit a self-move ------------------------------------
  (func $sel (param $c i32) (param $a i32) (param $b i32) (result i32)
    local.get $a
    local.get $b
    local.get $c
    select)

  ;; --- call_indirect through a real table/element segment -----------------
  (func $add_one (type $unary)
    local.get 0
    i32.const 1
    i32.add)

  (func $double_it (type $unary)
    local.get 0
    i32.const 2
    i32.mul)

  (func $apply_indirect (param $idx i32) (param $v i32) (result i32)
    local.get $v
    local.get $idx
    call_indirect (type $unary))

  (func $tail_caller (param $x i32) (result i32)
    local.get $x
    return_call $add_one)

  ;; --- typed function references / call_ref --------------------------------
  (func $apply_ref (param $f (ref null $unary)) (param $v i32) (result i32)
    local.get $v
    local.get $f
    call_ref $unary)

  (func $ref_test (result i32)
    ref.func $add_one
    ref.is_null)

  ;; --- float constants should print as floats ------------------------------
  (func $float32_const (result f32)
    f32.const 1.5)

  (func $float64_const (result f64)
    f64.const 3.141592653589793)

  ;; --- i32.wrap_i64 / 4-byte subregister write -----------------------------
  (func $wrap64 (param $x i64) (result i32)
    local.get $x
    i32.wrap_i64)

  ;; --- br_table -------------------------------------------------------------
  (func $br_table (param $x i32) (result i32)
    (local $r i32)
    block $exit
      block $c2
        block $c1
          block $c0
            local.get $x
            br_table $c0 $c1 $c2 $exit
          end
          i32.const 10
          local.set $r
          br $exit
        end
        i32.const 20
        local.set $r
        br $exit
      end
      i32.const 30
      local.set $r
    end
    local.get $r)

  ;; --- memory and bulk memory ----------------------------------------------
  (func $load_store (param $addr i32) (param $v i32) (result i32)
    local.get $addr
    local.get $v
    i32.store
    local.get $addr
    i32.load)

  (func $bulk (result i32)
    i32.const 0
    i32.const 0xAB
    i32.const 4
    memory.fill
    i32.const 8
    i32.const 0
    i32.const 4
    memory.copy
    i32.const 8
    i32.load)

  (func $init (result i32)
    i32.const 16
    i32.const 0
    i32.const 4
    memory.init 0
    i32.const 16
    i32.load)

  ;; --- block parameters and multi-value block results ----------------------
  (func $block_param (param $x i32) (result i32)
    local.get $x
    block (type $block_unary)
      i32.const 1
      i32.add
    end)

  (func $multi_block (type $pair)
    block (type $pair)
      i32.const 11
      i32.const 22
    end)

  ;; --- v128 transport, select, and deep-stack SIMD spill paths -------------
  (func $v128_id (param $v v128) (result v128)
    local.get $v)

  (func $v128_select (param $c i32) (param $a v128) (param $b v128) (result v128)
    local.get $a
    local.get $b
    local.get $c
    select (result v128))

  (func $simd_spill
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    v128.const i32x4 1 2 3 4
    v128.const i32x4 5 6 7 8
    i32x4.add
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop)

  (func $simd_scalar
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    v128.const i32x4 1 2 3 4
    i32x4.all_true
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop
    drop)

  ;; --- one-result intrinsics: clz / popcnt ---------------------------------
  (func $clz (param $x i32) (result i32)
    local.get $x
    i32.clz)

  (func $popcnt (param $x i32) (result i32)
    local.get $x
    i32.popcnt)

  (export "empty_then" (func $empty_then))
  (export "brif_else" (func $brif_else))
  (export "uninit_local" (func $uninit_local))
  (export "g0_set_const" (func $g0_set_const))
  (export "g0_via_local" (func $g0_via_local))
  (export "g1_bump" (func $g1_bump))
  (export "g2_bump" (func $g2_bump))
  (export "multi3" (func $multi3))
  (export "call_multi" (func $call_multi))
  (export "many_args" (func $many_args))
  (export "deep_stack" (func $deep_stack))
  (export "sel" (func $sel))
  (export "apply_indirect" (func $apply_indirect))
  (export "apply_ref" (func $apply_ref))
  (export "ref_test" (func $ref_test))
  (export "float32_const" (func $float32_const))
  (export "float64_const" (func $float64_const))
  (export "wrap64" (func $wrap64))
  (export "br_table" (func $br_table))
  (export "load_store" (func $load_store))
  (export "bulk" (func $bulk))
  (export "init" (func $init))
  (export "block_param" (func $block_param))
  (export "multi_block" (func $multi_block))
  (export "v128_id" (func $v128_id))
  (export "v128_select" (func $v128_select))
  (export "simd_spill" (func $simd_spill))
  (export "simd_scalar" (func $simd_scalar))
  (export "clz" (func $clz))
  (export "popcnt" (func $popcnt)))
