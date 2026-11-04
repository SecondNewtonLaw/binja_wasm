//
// Minimal WebAssembly binary encoder used by the lift tests.
//
// It only covers what the tests need: types, functions, one memory, globals,
// exports, data segments and (optionally) a funcref table.
//
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wasmbuild {

    using bytes = std::vector<uint8_t>;

    inline void putUleb(bytes &out, uint64_t v) {
        do {
            uint8_t b = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            if (v)
                b |= 0x80;
            out.push_back(b);
        } while (v);
    }

    inline void putSleb(bytes &out, int64_t v) {
        bool more = true;
        while (more) {
            uint8_t b = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            const bool sign = (b & 0x40) != 0;
            more = !((v == 0 && !sign) || (v == -1 && sign));
            if (more)
                b |= 0x80;
            out.push_back(b);
        }
    }

    inline void putBytes(bytes &out, const bytes &v) { out.insert(out.end(), v.begin(), v.end()); }

    inline void putRaw(bytes &out, const void *p, size_t n) {
        const auto *b = static_cast<const uint8_t *>(p);
        out.insert(out.end(), b, b + n);
    }

    struct Code {
        bytes b;

        Code &u8(uint8_t v) {
            b.push_back(v);
            return *this;
        }
        Code &uleb(uint64_t v) {
            putUleb(b, v);
            return *this;
        }
        Code &sleb(int64_t v) {
            putSleb(b, v);
            return *this;
        }
        Code &raw(const bytes &v) {
            putBytes(b, v);
            return *this;
        }
        Code &raw(const void *p, size_t n) {
            putRaw(b, p, n);
            return *this;
        }

        Code &op(uint8_t o) { return u8(o); }
        Code &prefix(uint8_t p, uint32_t sub) { return u8(p).uleb(sub); }

        Code &localGet(uint32_t i) { return op(0x20).uleb(i); }
        Code &localSet(uint32_t i) { return op(0x21).uleb(i); }
        Code &localTee(uint32_t i) { return op(0x22).uleb(i); }
        Code &globalGet(uint32_t i) { return op(0x23).uleb(i); }
        Code &globalSet(uint32_t i) { return op(0x24).uleb(i); }
        Code &tableGet(uint32_t i) { return op(0x25).uleb(i); }
        Code &tableSet(uint32_t i) { return op(0x26).uleb(i); }

        Code &i32(int32_t v) { return op(0x41).sleb(v); }
        Code &i64(int64_t v) { return op(0x42).sleb(v); }
        Code &f32(float v) {
            const uint32_t bits = std::bit_cast<uint32_t>(v);
            return op(0x43).raw(&bits, 4);
        }
        Code &f64(double v) {
            const uint64_t bits = std::bit_cast<uint64_t>(v);
            return op(0x44).raw(&bits, 8);
        }
        Code &f32Bits(uint32_t bits) { return op(0x43).raw(&bits, 4); }
        Code &f64Bits(uint64_t bits) { return op(0x44).raw(&bits, 8); }

        Code &drop() { return op(0x1A); }
        Code &select() { return op(0x1B); }
        Code &selectT(const std::vector<uint8_t> &types) {
            op(0x1C).uleb(types.size());
            for (uint8_t t : types)
                u8(t);
            return *this;
        }

        Code &block(int64_t type) { return op(0x02).sleb(type); }
        Code &loop(int64_t type) { return op(0x03).sleb(type); }
        Code &if_(int64_t type) { return op(0x04).sleb(type); }
        Code &else_() { return op(0x05); }
        Code &end() { return op(0x0B); }
        Code &br(uint32_t label) { return op(0x0C).uleb(label); }
        Code &brIf(uint32_t label) { return op(0x0D).uleb(label); }
        Code &brTable(const std::vector<uint32_t> &labels, uint32_t def) {
            op(0x0E).uleb(labels.size());
            for (uint32_t l : labels)
                uleb(l);
            return uleb(def);
        }
        Code &ret() { return op(0x0F); }
        Code &call(uint32_t f) { return op(0x10).uleb(f); }
        Code &callIndirect(uint32_t type, uint32_t table = 0) { return op(0x11).uleb(type).uleb(table); }
        Code &returnCall(uint32_t f) { return op(0x12).uleb(f); }
        Code &returnCallIndirect(uint32_t type, uint32_t table = 0) { return op(0x13).uleb(type).uleb(table); }

        Code &memarg(uint8_t o, uint32_t align, uint32_t offset) { return op(o).uleb(align).uleb(offset); }
        Code &load(uint8_t o, uint32_t offset = 0, uint32_t align = 0) { return memarg(o, align, offset); }
        Code &store(uint8_t o, uint32_t offset = 0, uint32_t align = 0) { return memarg(o, align, offset); }
        Code &memorySize(uint32_t mem = 0) { return op(0x3F).uleb(mem); }
        Code &memoryGrow(uint32_t mem = 0) { return op(0x40).uleb(mem); }

        Code &refNull(uint8_t heap = 0x70) { return op(0xD0).sleb(heap); }
        Code &refIsNull() { return op(0xD1); }
        Code &refFunc(uint32_t f) { return op(0xD2).uleb(f); }
        Code &refEq() { return op(0xD3); }

        // SIMD
        Code &v128Const(const bytes &v16) {
            prefix(0xFD, 0x0C);
            return raw(v16);
        }
        Code &shuffle(const bytes &v16) {
            prefix(0xFD, 0x0D);
            return raw(v16);
        }
        Code &simdMem(uint32_t sub, uint32_t offset, uint32_t align = 0) {
            prefix(0xFD, sub).uleb(align).uleb(offset);
            return *this;
        }
        Code &simdMemLane(uint32_t sub, uint32_t offset, uint8_t lane, uint32_t align = 0) {
            prefix(0xFD, sub).uleb(align).uleb(offset).u8(lane);
            return *this;
        }
        Code &simd(uint32_t sub) { return prefix(0xFD, sub); }
        Code &simdLane(uint32_t sub, uint8_t lane) { return prefix(0xFD, sub).u8(lane); }

        // atomics
        Code &atomicMem(uint32_t sub, uint32_t offset, uint32_t align = 0) {
            prefix(0xFE, sub).uleb(align).uleb(offset);
            return *this;
        }
        Code &atomicFence() { return prefix(0xFE, 3).u8(0); }
    };

    struct TypeDef {
        std::vector<uint8_t> params;
        std::vector<uint8_t> results;
    };

    struct FuncDef {
        uint32_t type = 0;
        std::vector<uint8_t> locals;
        bytes body;
        std::string exportName;
    };

    struct GlobalDef {
        uint8_t type = 0x7F;
        uint64_t init = 0;
        bool mut = true;
    };

    struct DataDef {
        uint32_t offset = 0;
        bytes data;
    };

    class ModuleBuilder {
      public:
        uint32_t addType(std::vector<uint8_t> params, std::vector<uint8_t> results) {
            types_.push_back({std::move(params), std::move(results)});
            return static_cast<uint32_t>(types_.size() - 1);
        }

        uint32_t addFunction(uint32_t type, bytes body, std::vector<uint8_t> locals = {}, std::string exportName = {}) {
            funcs_.push_back({type, std::move(locals), std::move(body), std::move(exportName)});
            return static_cast<uint32_t>(funcs_.size() - 1);
        }

        uint32_t
        addTypedFunction(std::vector<uint8_t> params, std::vector<uint8_t> results, bytes body, std::vector<uint8_t> locals = {}, std::string exportName = {}) {
            const uint32_t t = addType(std::move(params), std::move(results));
            return addFunction(t, std::move(body), std::move(locals), std::move(exportName));
        }

        uint32_t addGlobal(uint8_t type, uint64_t init, bool mut = true) {
            globals_.push_back({type, init, mut});
            return static_cast<uint32_t>(globals_.size() - 1);
        }

        void addData(uint32_t offset, bytes data) { data_.push_back({offset, std::move(data)}); }

        void addTable(uint32_t min, std::vector<uint32_t> elems) {
            tableMin_ = min;
            tableElems_ = std::move(elems);
            hasTable_ = true;
        }

        void setMemoryPages(uint32_t pages) { memoryPages_ = pages; }

        const std::vector<FuncDef> &functions() const { return funcs_; }

        bytes build() const {
            bytes out = {0x00, 'a', 's', 'm', 0x01, 0x00, 0x00, 0x00};

            // Type section.
            bytes typeSec;
            putUleb(typeSec, types_.size());
            for (const TypeDef &t : types_) {
                typeSec.push_back(0x60);
                putUleb(typeSec, t.params.size());
                putBytes(typeSec, t.params);
                putUleb(typeSec, t.results.size());
                putBytes(typeSec, t.results);
            }
            section(out, 1, typeSec);

            // Function section.
            bytes funcSec;
            putUleb(funcSec, funcs_.size());
            for (const FuncDef &f : funcs_)
                putUleb(funcSec, f.type);
            section(out, 3, funcSec);

            // Table section (only when call_indirect tests need one).
            if (hasTable_) {
                bytes tableSec;
                putUleb(tableSec, 1);
                tableSec.push_back(0x70); // funcref
                tableSec.push_back(0x00); // limits, min only
                putUleb(tableSec, tableMin_);
                section(out, 4, tableSec);
            }

            // Memory section.
            bytes memSec;
            putUleb(memSec, 1);
            memSec.push_back(0x00);
            putUleb(memSec, memoryPages_);
            section(out, 5, memSec);

            // Global section.
            if (!globals_.empty()) {
                bytes globSec;
                putUleb(globSec, globals_.size());
                for (const GlobalDef &g : globals_) {
                    globSec.push_back(g.type);
                    globSec.push_back(g.mut ? 1 : 0);
                    switch (g.type) {
                    case 0x7F:
                        globSec.push_back(0x41);
                        putSleb(globSec, static_cast<int32_t>(g.init));
                        break;
                    case 0x7E:
                        globSec.push_back(0x42);
                        putSleb(globSec, static_cast<int64_t>(g.init));
                        break;
                    case 0x7D: {
                        globSec.push_back(0x43);
                        const uint32_t bits = static_cast<uint32_t>(g.init);
                        putRaw(globSec, &bits, 4);
                        break;
                    }
                    case 0x7C: {
                        globSec.push_back(0x44);
                        putRaw(globSec, &g.init, 8);
                        break;
                    }
                    default:
                        globSec.push_back(0xD0);
                        putSleb(globSec, 0x70);
                        break;
                    }
                    globSec.push_back(0x0B);
                }
                section(out, 6, globSec);
            }

            // Export section.
            bytes expSec;
            size_t exports = 0;
            for (const FuncDef &f : funcs_)
                if (!f.exportName.empty())
                    ++exports;
            putUleb(expSec, exports);
            for (size_t i = 0; i < funcs_.size(); ++i) {
                if (funcs_[i].exportName.empty())
                    continue;
                putUleb(expSec, funcs_[i].exportName.size());
                putBytes(expSec, bytes(funcs_[i].exportName.begin(), funcs_[i].exportName.end()));
                expSec.push_back(0x00);
                putUleb(expSec, i);
            }
            if (exports)
                section(out, 7, expSec);

            // Element section (active, table 0, function indices).
            if (hasTable_ && !tableElems_.empty()) {
                bytes elemSec;
                putUleb(elemSec, 1);
                elemSec.push_back(0x00); // active, table 0
                elemSec.push_back(0x41);
                putSleb(elemSec, 0);
                elemSec.push_back(0x0B);
                putUleb(elemSec, tableElems_.size());
                for (uint32_t f : tableElems_)
                    putUleb(elemSec, f);
                section(out, 9, elemSec);
            }

            // Code section.
            bytes codeSec;
            putUleb(codeSec, funcs_.size());
            for (const FuncDef &f : funcs_) {
                bytes body;
                // Group consecutive locals with the same type.
                std::vector<std::pair<uint32_t, uint8_t>> groups;
                for (uint8_t t : f.locals) {
                    if (!groups.empty() && groups.back().second == t)
                        ++groups.back().first;
                    else
                        groups.push_back({1, t});
                }
                putUleb(body, groups.size());
                for (auto [count, type] : groups) {
                    putUleb(body, count);
                    body.push_back(type);
                }
                putBytes(body, f.body);
                putUleb(codeSec, body.size());
                putBytes(codeSec, body);
            }
            section(out, 10, codeSec);

            // Data section.
            if (!data_.empty()) {
                bytes dataSec;
                putUleb(dataSec, data_.size());
                for (const DataDef &d : data_) {
                    dataSec.push_back(0x00);
                    dataSec.push_back(0x41);
                    putSleb(dataSec, static_cast<int32_t>(d.offset));
                    dataSec.push_back(0x0B);
                    putUleb(dataSec, d.data.size());
                    putBytes(dataSec, d.data);
                }
                section(out, 11, dataSec);
            }

            return out;
        }

      private:
        static void section(bytes &out, uint8_t id, const bytes &payload) {
            out.push_back(id);
            putUleb(out, payload.size());
            putBytes(out, payload);
        }

        std::vector<TypeDef> types_;
        std::vector<FuncDef> funcs_;
        std::vector<GlobalDef> globals_;
        std::vector<DataDef> data_;
        uint32_t tableMin_ = 0;
        std::vector<uint32_t> tableElems_;
        bool hasTable_ = false;
        uint32_t memoryPages_ = 1;
    };

} // namespace wasmbuild
