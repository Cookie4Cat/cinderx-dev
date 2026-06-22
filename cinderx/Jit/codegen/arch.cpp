// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/arch.h"

#include <utility>

#if defined(CINDER_AARCH64)

using namespace asmjit;

namespace jit::codegen::arch {

namespace {

constexpr uint64_t kAddSubImmMask = 0xfff;
constexpr uint64_t kAddSubImmShift = 12;
constexpr uint64_t kMaxSplitAddSubImm =
    kAddSubImmMask + (kAddSubImmMask << kAddSubImmShift);

std::optional<std::pair<uint32_t, uint32_t>> split_add_sub_imm(uint64_t imm) {
  if (imm == 0 || imm > kMaxSplitAddSubImm || arm::Utils::isAddSubImm(imm)) {
    return std::nullopt;
  }

  auto hi = static_cast<uint32_t>(imm >> kAddSubImmShift);
  auto lo = static_cast<uint32_t>(imm & kAddSubImmMask);
  if (hi == 0 || hi > kAddSubImmMask || lo == 0) {
    return std::nullopt;
  }

  return std::make_pair(hi, lo);
}

arch::Gp scratch_for_cmp(const arch::Gp& reg) {
  arch::Gp scratch =
      reg.id() == arch::reg_scratch_0.id() ? arch::reg_scratch_1
                                           : arch::reg_scratch_0;
  if (reg.isGpW()) {
    scratch = scratch.w();
  }
  return scratch;
}

} // namespace

bool is_split_add_sub_imm(uint64_t imm) {
  return split_add_sub_imm(imm).has_value();
}

// Attempt to build a pointer using an offset from a base register. If it is
// not possible to do so, return std::nullopt.
static std::optional<a64::Mem>
ptr_offset_try(const a64::Gp& base, int32_t offset, AccessSize access_size) {
  if (offset >= -256 && offset < 256) {
    // Unscaled immediate offset
    return a64::ptr(base, offset);
  }

  if (offset >= 0 && (offset & (static_cast<int32_t>(access_size) - 1)) == 0 &&
      offset < static_cast<int32_t>(access_size) * 4096) {
    // Unsigned scaled immediate
    return a64::ptr(base, offset);
  }

  return std::nullopt;
}

// Build a pointer using an offset from a base register. If it is not possible
// to do so, terminate the program. This should only be used in contexts where
// the offset is known to be within range or it is not possible to access a
// builder.
a64::Mem
ptr_offset(const a64::Gp& base, int32_t offset, AccessSize access_size) {
  auto opt = ptr_offset_try(base, offset, access_size);
  JIT_CHECK(opt.has_value(), "offset out of range");
  return opt.value();
}

// a64 has a couple of addressing modes, and we want to use the one with the
// fewest instructions possible. In the best case, this will no instructions
// because we can address it directly. Alternatively we could add/sub an offset
// into a scratch register and address that. Finally, in the worst case this
// will be 4 movz/movk instructions, then an indirect pointer through a
// register.
a64::Mem ptr_resolve(
    a64::Builder* as,
    const a64::Gp& base,
    int32_t offset,
    const a64::Gp& scratch,
    AccessSize access_size) {
  if (auto opt = ptr_offset_try(base, offset, access_size)) {
    return opt.value();
  }

  if (offset >= 0 && arm::Utils::isAddSubImm(static_cast<uint64_t>(offset))) {
    // Add immediate
    as->add(scratch, base, offset);
    return a64::ptr(scratch);
  }
  if (offset < 0 && arm::Utils::isAddSubImm(static_cast<uint64_t>(-offset))) {
    // Sub immediate
    as->sub(scratch, base, -offset);
    return a64::ptr(scratch);
  }

  // Base + index
  as->mov(scratch, offset);
  return a64::ptr(base, scratch);
}

void cmp_immediate(a64::Builder* as, const arch::Gp& reg, uint64_t imm) {
  if (arm::Utils::isAddSubImm(imm)) {
    as->cmp(reg, imm);
  } else if (arm::Utils::isAddSubImm(-imm)) {
    as->cmn(reg, -imm);
  } else {
    arch::Gp scratch = scratch_for_cmp(reg);

    as->mov(scratch, imm);
    as->cmp(reg, scratch);
  }
}

void cmp_immediate_for_equality(
    a64::Builder* as,
    const arch::Gp& reg,
    uint64_t imm) {
  if (arm::Utils::isAddSubImm(imm) || arm::Utils::isAddSubImm(-imm)) {
    cmp_immediate(as, reg, imm);
  } else if (auto split = split_add_sub_imm(imm)) {
    auto [hi, lo] = *split;
    arch::Gp scratch = scratch_for_cmp(reg);
    as->sub(scratch, reg, hi, a64::lsl(kAddSubImmShift));
    as->cmp(scratch, lo);
  } else if (auto split = split_add_sub_imm(-imm)) {
    auto [hi, lo] = *split;
    arch::Gp scratch = scratch_for_cmp(reg);
    as->add(scratch, reg, hi, a64::lsl(kAddSubImmShift));
    as->cmn(scratch, lo);
  } else {
    cmp_immediate(as, reg, imm);
  }
}

void add_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    uint64_t rhsi) {
  if (rhsi == 0) {
    if (res != lhs) {
      as->mov(res, lhs);
    }
  } else if (arm::Utils::isAddSubImm(rhsi)) {
    as->add(res, lhs, rhsi);
  } else if (auto split = split_add_sub_imm(rhsi)) {
    auto [hi, lo] = *split;
    as->add(res, lhs, hi, a64::lsl(kAddSubImmShift));
    as->add(res, res, lo);
  } else {
    as->mov(arch::reg_scratch_0, rhsi);
    as->add(res, lhs, arch::reg_scratch_0);
  }
}

void sub_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    uint64_t rhsi) {
  if (rhsi == 0) {
    if (res != lhs) {
      as->mov(res, lhs);
    }
  } else if (arm::Utils::isAddSubImm(rhsi)) {
    as->sub(res, lhs, rhsi);
  } else if (auto split = split_add_sub_imm(rhsi)) {
    auto [hi, lo] = *split;
    as->sub(res, lhs, hi, a64::lsl(kAddSubImmShift));
    as->sub(res, res, lo);
  } else {
    as->mov(arch::reg_scratch_0, rhsi);
    as->sub(res, lhs, arch::reg_scratch_0);
  }
}

void add_signed_immediate(
    a64::Builder* as,
    const a64::Gp& res,
    const a64::Gp& lhs,
    int64_t rhsi) {
  uint64_t rshu = static_cast<uint64_t>(rhsi);
  if (rhsi >= 0) {
    add_immediate(as, res, lhs, rshu);
  } else {
    sub_immediate(as, res, lhs, -rshu);
  }
}

} // namespace jit::codegen::arch

#endif

namespace jit::codegen {

std::ostream& operator<<(std::ostream& out, const PhyLocation& loc) {
  return out << loc.toString();
}

} // namespace jit::codegen
