#!/usr/bin/python3

import subprocess
import unittest


class TestSanity(unittest.TestCase):
    def setUp(self):
        self.help_output = subprocess.check_output(
            ["/usr/bin/makedumpfile", "-h"]
        ).decode()
        self.ldd_output = subprocess.check_output(
            ["ldd", "/usr/bin/makedumpfile"]
        ).decode()

    def test_lzo(self):
        self.assertIn("LZO support:\n  enabled", self.help_output)
        self.assertIn("liblzo2.so", self.ldd_output)

    def test_zstd(self):
        self.assertIn("zstd support:\n  enabled", self.help_output)
        self.assertIn("libzstd.so", self.ldd_output)


if __name__ == "__main__":
    unittest.main()
