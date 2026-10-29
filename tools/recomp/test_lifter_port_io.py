"""Direct x86 port writes are routed through observable runtime hooks."""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _lift(op_str, operands, address=0x0019A305):
    insn = Instruction(address, 1, "out", op_str, "ee", operands=operands)
    return " ".join(Lifter().lift_instruction(insn))


def _lift_in(op_str, operands, address=0x0019AF8D):
    insn = Instruction(address, 1, "in", op_str, "ec", operands=operands)
    return " ".join(Lifter().lift_instruction(insn))


class PortIoLifterTest(unittest.TestCase):

    def test_out_dx_al_routes_to_byte_hook_with_guest_va(self):
        out = _lift("dx, al", [_reg("dx"), _reg("al")])
        self.assertIn(
            "recomp_io_out8((uint16_t)(LO16(edx)), "
            "(uint8_t)(LO8(eax)), 0x0019A305u)", out)
        self.assertNotIn("RECOMP_UNIMPL", out)

    def test_immediate_port_and_wider_sources_select_their_width(self):
        out16 = _lift("0x20, ax", [_imm(0x20), _reg("ax")])
        out32 = _lift("0x21, eax", [_imm(0x21), _reg("eax")])
        self.assertIn(
            "recomp_io_out16((uint16_t)(0x20), (uint16_t)(LO16(eax))",
            out16)
        self.assertIn(
            "recomp_io_out32((uint16_t)(0x21), (uint32_t)(eax)", out32)

    def test_invalid_out_form_keeps_runtime_trap_marker(self):
        out = _lift("ecx, al", [_reg("ecx"), _reg("al")])
        self.assertIn(
            'RECOMP_UNIMPL("out ecx, al", 0x0019A305u)', out)

    def test_in_al_dx_routes_result_to_low_byte(self):
        out = _lift_in("al, dx", [_reg("al"), _reg("dx")])
        self.assertIn(
            "SET_LO8(eax, recomp_io_in8((uint16_t)(LO16(edx)), "
            "0x0019AF8Du));", out)
        self.assertNotIn("RECOMP_UNIMPL", out)

    def test_immediate_in_selects_destination_width(self):
        out16 = _lift_in("ax, 0x20", [_reg("ax"), _imm(0x20)])
        out32 = _lift_in("eax, 0x21", [_reg("eax"), _imm(0x21)])
        self.assertIn(
            "SET_LO16(eax, recomp_io_in16((uint16_t)(0x20)", out16)
        self.assertIn(
            "eax = recomp_io_in32((uint16_t)(0x21)", out32)


if __name__ == "__main__":
    unittest.main()
