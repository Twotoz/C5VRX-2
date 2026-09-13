import unittest
from analyze_linear80_pads import compare


class PadCompareTests(unittest.TestCase):
    def test_phase_wrap_and_stride(self):
        template=list(range(64))
        for step in (1,2):
            capture=[template[(61+i*step)%64] for i in range(256)]
            self.assertTrue(compare(capture,template,step)['exact'])
            capture[100]^=1
            self.assertEqual(compare(capture,template,step)['mismatch'],1)

    def test_no_alignment(self):
        self.assertFalse(compare([63]*32,list(range(64)),1)['aligned'])


if __name__=='__main__': unittest.main()
