#!/usr/bin/env python3
import argparse
import os
import sys

from seolc_compiler import CompileError, compile_source


def default_out_path(src_path: str) -> str:
    root, _ = os.path.splitext(src_path)
    return root + ".XEF"

def format_compile_error(path: str, source_text: str, err: CompileError) -> str:
    head = f"{path}:{err.line}:{err.col}: error: {err.message}"
    lines = source_text.splitlines()
    if err.line < 1 or err.line > len(lines):
        return head
    src = lines[err.line - 1]
    col = err.col
    if col < 1:
        col = 1
    caret = " " * (col - 1) + "^"
    return "\n".join([head, src, caret])


def main() -> int:
    parser = argparse.ArgumentParser(description="Host SeolC compiler (.XC -> Native .XEF)")
    parser.add_argument("src", help="source .XC file path")
    parser.add_argument("-o", "--out", help="output .XEF path")
    parser.add_argument("--target", default="native16", help="target ABI (only native16 is supported)")
    parser.add_argument("-I", dest="include_dirs", action="append", default=[], help="add include search path")
    args = parser.parse_args()

    src_path = args.src
    out_path = args.out if args.out else default_out_path(src_path)

    if args.target != "native16":
        print(f"seolc_host: unsupported target: {args.target}", file=sys.stderr)
        return 1

    try:
        with open(src_path, "r", encoding="utf-8", errors="replace") as f:
            src_text = f.read()
        src_dir = os.path.dirname(os.path.abspath(src_path))
        tool_dir = os.path.dirname(os.path.abspath(__file__))
        default_inc = [
            os.path.normpath(os.path.join(tool_dir, "..", "programs", "include")),
            os.path.normpath(os.path.join(tool_dir, "..", "include")),
        ]
        include_dirs: list[str] = [src_dir]
        include_dirs.extend(os.path.abspath(p) for p in args.include_dirs)
        for p in default_inc:
            if os.path.isdir(p):
                include_dirs.append(p)

        dedup_inc: list[str] = []
        seen_inc: set[str] = set()
        for p in include_dirs:
            if p in seen_inc:
                continue
            seen_inc.add(p)
            dedup_inc.append(p)

        image = compile_source(src_text, source_path=src_path, include_dirs=dedup_inc)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, "wb") as f:
            f.write(image)
    except FileNotFoundError:
        print(f"seolc_host: source not found: {src_path}", file=sys.stderr)
        return 1
    except CompileError as e:
        print(format_compile_error(src_path, src_text, e), file=sys.stderr)
        return 1
    except OSError as e:
        print(f"seolc_host: {e}", file=sys.stderr)
        return 1
    except Exception as e:
        print(f"seolc_host: internal error: {e}", file=sys.stderr)
        return 1

    print(f"seolc_host: built {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
