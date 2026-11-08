// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
void SHR(TranslatorVisitor& v, u64 insn, const IR::U32& shift) {
    union {
        u64 insn;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> src_reg_a;
        BitField<39, 1, u64> is_wrapped;
        BitField<40, 1, u64> brev;
        BitField<43, 1, u64> xmode;
        BitField<47, 1, u64> cc;
        BitField<48, 1, u64> is_signed;
    } const shr{insn};

    if (shr.xmode != 0) {
        throw NotImplementedException("SHR.XMODE");
    }
    if (shr.cc != 0) {
        throw NotImplementedException("SHR.CC");
    }

    IR::U32 base{v.X(shr.src_reg_a)};
    if (shr.brev == 1) {
        base = v.ir.BitReverse(base);
    }
    // Without .W, Maxwell saturates shifts of 32 or more: unsigned shifts give 0 and signed shifts
    // fill with the sign bit. SPIR-V leaves such shifts undefined (AMD masks the amount to 5 bits),
    // so clamp the amount before shifting rather than inspecting an undefined result.
    const IR::U32 safe_shift = shr.is_wrapped == 0 ? v.ir.UMin(shift, v.ir.Imm32(31))
                                                   : v.ir.BitwiseAnd(shift, v.ir.Imm32(31));
    IR::U32 result;
    if (shr.is_signed == 1) {
        // An arithmetic shift by 31 already is the sign fill.
        result = IR::U32{v.ir.ShiftRightArithmetic(base, safe_shift)};
    } else {
        result = IR::U32{v.ir.ShiftRightLogical(base, safe_shift)};
        if (shr.is_wrapped == 0) {
            const IR::U1 is_safe{v.ir.ILessThan(shift, v.ir.Imm32(32), false)};
            result = IR::U32{v.ir.Select(is_safe, result, v.ir.Imm32(0))};
        }
    }
    v.X(shr.dest_reg, result);
}
} // Anonymous namespace

void TranslatorVisitor::SHR_reg(u64 insn) {
    SHR(*this, insn, GetReg20(insn));
}

void TranslatorVisitor::SHR_cbuf(u64 insn) {
    SHR(*this, insn, GetCbuf(insn));
}

void TranslatorVisitor::SHR_imm(u64 insn) {
    SHR(*this, insn, GetImm20(insn));
}
} // namespace Shader::Maxwell
