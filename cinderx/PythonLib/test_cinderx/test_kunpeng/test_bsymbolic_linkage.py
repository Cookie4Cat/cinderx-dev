# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Validate the -Wl,-Bsymbolic-functions linkage of _cinderx.so.

The JIT runtime helpers (inline caches, frame setup, JITRT_*) call each
other on every attribute access and call made from JIT-compiled code.
_cinderx.so is linked with -Wl,-Bsymbolic-functions so those intra-DSO
calls bind directly instead of going through PLT thunks.

These tests machine-check the two invariants of that link flag:

1. No JUMP_SLOT relocation may resolve to a symbol that _cinderx.so
   itself defines (otherwise internal calls are back on the PLT and the
   flag silently regressed, e.g. dropped by a build-system change).
2. The dynamic export surface is preserved: binding internal calls must
   not stop PyInit__cinderx from being a globally visible definition.
"""

import platform
import struct
import sys
import unittest
from pathlib import Path

EM_X86_64 = 0x3E
EM_AARCH64 = 0xB7

R_JUMP_SLOT_BY_MACHINE = {
    EM_X86_64: 7,  # R_X86_64_JUMP_SLOT
    EM_AARCH64: 1026,  # R_AARCH64_JUMP_SLOT
}

SHT_RELA = 4
SHT_DYNSYM = 11
SHN_UNDEF = 0
STB_GLOBAL = 1


class _Elf64:
    """Minimal read-only ELF64 view: dynamic symbols and RELA relocations."""

    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if self.data[:4] != b"\x7fELF":
            raise unittest.SkipTest(f"{path} is not an ELF file")
        if self.data[4] != 2 or self.data[5] != 1:
            raise unittest.SkipTest(f"{path} is not little-endian ELF64")
        self.machine = struct.unpack_from("<H", self.data, 0x12)[0]

        e_shoff = struct.unpack_from("<Q", self.data, 0x28)[0]
        e_shentsize, e_shnum = struct.unpack_from("<HH", self.data, 0x3A)
        self.sections = []
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            (
                _name,
                sh_type,
                _flags,
                _addr,
                sh_offset,
                sh_size,
                sh_link,
                _info,
                _align,
                sh_entsize,
            ) = struct.unpack_from("<IIQQQQIIQQ", self.data, off)
            self.sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

        self.dynsym = next(
            (s for s in self.sections if s[0] == SHT_DYNSYM), None
        )
        if self.dynsym is None:
            raise unittest.SkipTest(f"{path} has no .dynsym section")
        self.dynstr = self.sections[self.dynsym[3]]

    def symbol(self, index: int) -> tuple[str, int, int]:
        """Return (name, binding, section index) of dynamic symbol `index`."""
        off = self.dynsym[1] + index * 24
        st_name, st_info, _other, st_shndx = struct.unpack_from(
            "<IBBH", self.data, off
        )
        end = self.data.index(b"\x00", self.dynstr[1] + st_name)
        name = self.data[self.dynstr[1] + st_name : end].decode()
        return name, st_info >> 4, st_shndx

    def iter_symbols(self):
        count = self.dynsym[2] // 24
        for i in range(count):
            yield self.symbol(i)

    def jump_slot_symbol_indexes(self):
        jump_slot = R_JUMP_SLOT_BY_MACHINE[self.machine]
        for sh_type, offset, size, _link, entsize in self.sections:
            if sh_type != SHT_RELA or entsize == 0:
                continue
            for off in range(offset, offset + size, entsize):
                _r_offset, r_info = struct.unpack_from("<QQ", self.data, off)
                if (r_info & 0xFFFFFFFF) == jump_slot:
                    yield r_info >> 32


def _cinderx_so_path() -> Path:
    try:
        import _cinderx
    except ImportError:
        raise unittest.SkipTest("_cinderx extension is not importable")
    so = getattr(_cinderx, "__file__", None)
    if not so:
        raise unittest.SkipTest("_cinderx has no extension file (statically linked?)")
    return Path(so)


@unittest.skipUnless(sys.platform.startswith("linux"), "ELF linkage is Linux-only")
class BsymbolicLinkageTests(unittest.TestCase):
    def setUp(self) -> None:
        self.elf = _Elf64(_cinderx_so_path())
        if self.elf.machine not in R_JUMP_SLOT_BY_MACHINE:
            self.skipTest(
                f"unsupported ELF machine: {hex(self.elf.machine)}"
                f" ({platform.machine()})"
            )

    def test_no_plt_indirection_for_locally_defined_symbols(self) -> None:
        # With -Wl,-Bsymbolic-functions every JUMP_SLOT (PLT) relocation
        # must reference an UNDEFINED symbol, i.e. one imported from
        # another DSO such as libpython. A JUMP_SLOT against a symbol
        # defined inside _cinderx.so means intra-DSO calls are routed
        # through the PLT again and the link flag has regressed.
        offenders = []
        for sym_index in self.elf.jump_slot_symbol_indexes():
            name, _binding, st_shndx = self.elf.symbol(sym_index)
            if st_shndx != SHN_UNDEF:
                offenders.append(name)
        self.assertEqual(
            offenders,
            [],
            "JUMP_SLOT relocations bound to symbols defined in _cinderx.so "
            "(PLT indirection for intra-DSO calls); was "
            "-Wl,-Bsymbolic-functions dropped from the link line? "
            f"First offenders: {offenders[:10]}",
        )

    def test_module_init_symbol_still_exported(self) -> None:
        # -Bsymbolic-functions only changes how internal references bind;
        # it must not shrink the dynamic export surface that CPython's
        # import machinery (dlopen + dlsym) relies on.
        for name, binding, st_shndx in self.elf.iter_symbols():
            if name == "PyInit__cinderx":
                self.assertNotEqual(
                    st_shndx, SHN_UNDEF, "PyInit__cinderx must be defined"
                )
                self.assertEqual(
                    binding, STB_GLOBAL, "PyInit__cinderx must have GLOBAL binding"
                )
                return
        self.fail("PyInit__cinderx not found in .dynsym of _cinderx.so")


if __name__ == "__main__":
    unittest.main()
