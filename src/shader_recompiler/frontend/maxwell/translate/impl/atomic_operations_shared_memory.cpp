// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
enum class AtomOp : u64 {
    ADD,
    MIN,
    MAX,
    INC,
    DEC,
    AND,
    OR,
    XOR,
    EXCH,
};

enum class AtomsSize : u64 {
    U32,
    S32,
    U64,
};

IR::U32U64 ApplyAtomsOp(IR::IREmitter& ir, const IR::U32& offset, const IR::U32U64& op_b, AtomOp op,
                        bool is_signed) {
    switch (op) {
    case AtomOp::ADD:
        return ir.SharedAtomicIAdd(offset, op_b);
    case AtomOp::MIN:
        return ir.SharedAtomicIMin(offset, op_b, is_signed);
    case AtomOp::MAX:
        return ir.SharedAtomicIMax(offset, op_b, is_signed);
    case AtomOp::INC:
        return ir.SharedAtomicInc(offset, op_b);
    case AtomOp::DEC:
        return ir.SharedAtomicDec(offset, op_b);
    case AtomOp::AND:
        return ir.SharedAtomicAnd(offset, op_b);
    case AtomOp::OR:
        return ir.SharedAtomicOr(offset, op_b);
    case AtomOp::XOR:
        return ir.SharedAtomicXor(offset, op_b);
    case AtomOp::EXCH:
        return ir.SharedAtomicExchange(offset, op_b);
    default:
        throw NotImplementedException("Integer Atoms Operation {}", op);
    }
}

IR::U32 AtomsOffset(TranslatorVisitor& v, u64 insn) {
    union {
        u64 raw;
        BitField<8, 8, IR::Reg> offset_reg;
        BitField<30, 22, u64> absolute_offset;
        BitField<30, 22, s64> relative_offset;
    } const encoding{insn};

    if (encoding.offset_reg == IR::Reg::RZ) {
        return v.ir.Imm32(static_cast<u32>(encoding.absolute_offset << 2));
    } else {
        const s32 relative{static_cast<s32>(encoding.relative_offset << 2)};
        return v.ir.IAdd(v.X(encoding.offset_reg), v.ir.Imm32(relative));
    }
}

void StoreResult(TranslatorVisitor& v, IR::Reg dest_reg, const IR::Value& result, AtomsSize size) {
    switch (size) {
    case AtomsSize::U32:
    case AtomsSize::S32:
        return v.X(dest_reg, IR::U32{result});
    case AtomsSize::U64:
        return v.L(dest_reg, IR::U64{result});
    default:
        break;
    }
}
} // Anonymous namespace

void TranslatorVisitor::ATOMS(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> addr_reg;
        BitField<20, 8, IR::Reg> src_reg_b;
        BitField<28, 2, AtomsSize> size;
        BitField<52, 4, AtomOp> op;
    } const atoms{insn};

    const bool size_64{atoms.size == AtomsSize::U64};
    if (size_64 && atoms.op != AtomOp::EXCH) {
        throw NotImplementedException("64-bit Atoms Operation {}", atoms.op.Value());
    }
    const bool is_signed{atoms.size == AtomsSize::S32};
    const IR::U32 offset{AtomsOffset(*this, insn)};

    IR::Value result;
    if (size_64) {
        result = ApplyAtomsOp(ir, offset, L(atoms.src_reg_b), atoms.op, is_signed);
    } else {
        result = ApplyAtomsOp(ir, offset, X(atoms.src_reg_b), atoms.op, is_signed);
    }
    StoreResult(*this, atoms.dest_reg, result, atoms.size);
}

// Only the shared compare-and-swap that the Ampere lowering emits for ATOMS.CAS.32 (opcode 0x38d,
// lower_to_maxwell.h), in that lowering's own encoding: dest 0..7, address 8..15, compare
// 20..27, signed word offset 28..41, new value 42..49, and LOWERED_CAS_MARK in 50..55. The real
// Maxwell ATOMS.CAS encoding is still not implemented.
void TranslatorVisitor::ATOMS_cas(u64 insn) {
    constexpr u64 LOWERED_CAS_MARK = 0x2d;
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> addr_reg;
        BitField<20, 8, IR::Reg> compare_reg;
        BitField<28, 14, s64> word_offset;
        BitField<42, 8, IR::Reg> value_reg;
        BitField<50, 6, u64> mark;
    } const cas{insn};
    if (cas.mark != LOWERED_CAS_MARK) {
        throw NotImplementedException("Instruction ATOMS (cas) is not implemented");
    }
    const s32 byte_offset{static_cast<s32>(cas.word_offset.Value() * 4)};
    IR::U32 offset{ir.Imm32(static_cast<u32>(byte_offset))};
    if (cas.addr_reg != IR::Reg::RZ) {
        offset = ir.IAdd(X(cas.addr_reg), ir.Imm32(byte_offset));
    }
    X(cas.dest_reg, ir.SharedAtomicCompareExchange(offset, X(cas.compare_reg), X(cas.value_reg)));
}

} // namespace Shader::Maxwell
