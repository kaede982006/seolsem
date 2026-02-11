import struct
import tempfile
import unittest
from pathlib import Path

from seolc_compiler import CompileError, compile_source


def parse_header(blob: bytes):
    return struct.unpack("<4s12H", blob[:28])


class SeolcCompilerTests(unittest.TestCase):
    def test_minimal_program(self):
        src = "int main(){ return 0; }"
        blob = compile_source(src)
        h = parse_header(blob)
        magic, ver, flags, entry, code, data, bss, imp_off, imp_cnt, rel_off, rel_cnt, str_off, str_sz = h
        self.assertEqual(magic, b"XEFN")
        self.assertEqual(ver, 1)
        self.assertEqual(flags, 0)
        self.assertGreater(code, 0)
        self.assertEqual(data, 0)
        self.assertEqual(bss, 0)
        self.assertEqual(imp_cnt, 0)
        self.assertEqual(rel_cnt, 0)
        self.assertEqual(entry, 0)
        self.assertEqual(imp_off, 0)
        self.assertEqual(rel_off, 0)
        self.assertEqual(str_off, 0)
        self.assertEqual(str_sz, 0)

    def test_import_and_relocation(self):
        src = "int main(){ print(\"abc\"); return 0; }"
        blob = compile_source(src)
        h = parse_header(blob)
        _, _, _, _, code, data, _, imp_off, imp_cnt, rel_off, rel_cnt, str_off, str_sz = h
        self.assertGreater(code, 0)
        self.assertGreater(data, 0)
        self.assertGreater(imp_cnt, 0)
        self.assertGreater(rel_cnt, 0)
        self.assertGreater(imp_off, 0)
        self.assertGreater(rel_off, 0)
        self.assertGreater(str_off, 0)
        self.assertGreater(str_sz, 0)

    def test_comparison_bool_codegen_short_jump_layout(self):
        src = "int main(){ return (1 <= 1); }"
        blob = compile_source(src)
        h = parse_header(blob)
        code_size = h[4]
        code = blob[28:28 + code_size]

        # Regression: jcc offset for bool materialization must land at mov ax,1.
        # Expected layout: jcc +2 ; jmp +3 ; mov ax,1
        self.assertIn(bytes([0x7E, 0x02, 0xEB, 0x03, 0xB8, 0x01, 0x00]), code)

    def test_control_flow_and_functions(self):
        src = """
int add(int a, int b) { return a + b; }
int main() {
  int i = 0;
  int s = 0;
  for (i = 0; i < 5; i = i + 1) {
    s = add(s, i);
  }
  if (s > 0) {
    print(s);
  }
  return s;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 32)

    def test_pointer_syntax(self):
        src = """
int main() {
  int x = 7;
  int *p;
  p = &x;
  *p = *p + 1;
  return x;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")

    def test_char_index_load_zero_extend(self):
        src = """
int main() {
  char *s = "AB";
  return s[1];
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        code_size = h[4]
        code = blob[28:28 + code_size]

        # Regression: char must be 1 byte and loads must be zero-extended to AX.
        # mov al, [bx]; mov ah, 0
        self.assertIn(bytes([0x8A, 0x07, 0xB4, 0x00]), code)

    def test_struct_member_access(self):
        src = """
struct Pair { int a; int b; };
int main() {
  struct Pair p;
  struct Pair *q;
  p.a = 3;
  p.b = 9;
  q = &p;
  q->a = q->a + p.b;
  return p.a;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 32)

    def test_struct_assignment(self):
        src = """
struct Pair { int a; int b; };
int main() {
  struct Pair p;
  struct Pair q;
  p.a = 1;
  p.b = 2;
  q = p;
  return q.b;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 32)

    def test_array_indexing(self):
        src = """
int main() {
  int a[4];
  int i;
  i = 2;
  a[0] = 10;
  a[i] = a[0] + 5;
  return a[2];
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 32)

    def test_global_and_multidim_array(self):
        src = """
int g;
int m[2][3];
int main() {
  g = 7;
  m[1][2] = g + 5;
  return m[1][2];
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[6], 0)  # bss

    def test_nested_struct_field_and_copy(self):
        src = """
struct Inner { int x; int y; };
struct Outer { struct Inner a; struct Inner b; };
int main() {
  struct Outer o;
  struct Inner t;
  o.a.x = 3;
  o.a.y = 9;
  o.b = o.a;
  t = o.b;
  return t.y;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 32)

    def test_preprocess_define(self):
        src = """
#define TEN 10
int main() {
  return TEN;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_include(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inc = root / "defs.xh"
            srcf = root / "main.xc"
            inc.write_text("#define INCVAL 33\n", encoding="utf-8")
            srcf.write_text('#include "defs.xh"\nint main(){ return INCVAL; }\n', encoding="utf-8")

            blob = compile_source(srcf.read_text(encoding="utf-8"), source_path=str(srcf), include_dirs=[str(root)])
            h = parse_header(blob)
            self.assertEqual(h[0], b"XEFN")
            self.assertGreater(h[4], 0)

    def test_preprocess_include_angle(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            inc = root / "sys.xh"
            srcf = root / "main.xc"
            inc.write_text("#define SYSVAL 41\n", encoding="utf-8")
            srcf.write_text("#include <sys.xh>\nint main(){ return SYSVAL; }\n", encoding="utf-8")

            blob = compile_source(srcf.read_text(encoding="utf-8"), source_path=str(srcf), include_dirs=[str(root)])
            h = parse_header(blob)
            self.assertEqual(h[0], b"XEFN")
            self.assertGreater(h[4], 0)

    def test_array_initializer_local(self):
        src = """
int main() {
  int a[2][3] = {{1, 2, 3}, {4, 5, 6}};
  return a[1][2];
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_array_initializer_global(self):
        src = """
int g = 3;
int a[3] = {1, 2, g};
int main() {
  return a[2];
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_function_like_macro(self):
        src = """
#define ADD(x,y) ((x) + (y))
int main() {
  return ADD(4, 5);
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_macro_rescan_alias(self):
        src = """
#define INC(x) ((x) + 1)
#define ALIAS INC
int main() {
  return ALIAS(7);
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_conditionals(self):
        src = """
#define FLAG 1
#if FLAG
int main() { return 11; }
#else
int main() { return 22; }
#endif
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_struct_initializer_local(self):
        src = """
struct Pair { int a; int b; };
int main() {
  struct Pair p = {3, 9};
  return p.b;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_struct_initializer_global(self):
        src = """
int g = 4;
struct Pair { int a; int b; };
struct Pair p = {g, 7};
int main() {
  return p.a + p.b;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_struct_member_and_element_value_expr(self):
        src = """
struct Inner { int x; int y; };
struct Outer { struct Inner a; };
int main() {
  struct Outer o;
  struct Inner arr[2];
  struct Inner m = {0, 0};
  struct Inner n = {0, 0};
  o.a.x = 11;
  o.a.y = 22;
  arr[1].x = 33;
  arr[1].y = 44;
  m = o.a;
  n = arr[1];
  return m.y + n.x;
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_variadic_macro(self):
        src = """
#define SUM2(a, ...) ((a) + (__VA_ARGS__))
int main() {
  return SUM2(10, 5);
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_preprocess_stringize_and_concat(self):
        src = """
#define STR(x) #x
#define CAT(a,b) a##b
int main() {
  int xy = 7;
  print(STR(hello_world));
  return CAT(x, y);
}
"""
        blob = compile_source(src)
        h = parse_header(blob)
        self.assertEqual(h[0], b"XEFN")
        self.assertGreater(h[4], 0)

    def test_error_has_line_col(self):
        src = "int main(){ y = 1; return 0; }"
        with self.assertRaises(CompileError) as cm:
            compile_source(src)
        err = cm.exception
        self.assertGreaterEqual(err.line, 1)
        self.assertGreaterEqual(err.col, 1)


if __name__ == "__main__":
    unittest.main()
