"""Cache-management instructions must not become privileged host ops."""

import unittest

from .disasm import Instruction
from .lifter import Lifter


class CacheControlLifterTest(unittest.TestCase):

    def test_wbinvd_becomes_attributable_host_fence(self):
        insn = Instruction(
            0x001995AF, 2, "wbinvd", "", "0f09", operands=[]
        )
        lifter = Lifter()
        out = " ".join(lifter.lift_instruction(insn))

        self.assertIn("recomp_memory_fence(0x001995AFu);", out)
        self.assertIn("host-coherent cache fence", out)
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertEqual(lifter.unimplemented, {})


if __name__ == "__main__":
    unittest.main()
