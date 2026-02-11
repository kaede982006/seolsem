import unittest

from seolc_compiler.errors import CompileError
from seolc_host import format_compile_error


class SeolcHostTests(unittest.TestCase):
    def test_format_compile_error_with_caret(self):
        src = "int main(){\n  bad = 1;\n}\n"
        err = CompileError(2, 3, "use of undefined variable 'bad'")
        text = format_compile_error("programs/src/TEST.XC", src, err)

        self.assertIn("programs/src/TEST.XC:2:3: error: use of undefined variable 'bad'", text)
        self.assertIn("  bad = 1;", text)
        self.assertIn("  ^", text)


if __name__ == "__main__":
    unittest.main()
