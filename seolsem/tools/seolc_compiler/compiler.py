import os
import re
from dataclasses import dataclass

from .codegen_x86_16 import generate
from .errors import CompileError
from .lexer import Lexer
from .parser import Parser
from .sema import analyze
from .xef_native_pack import pack_native


_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass
class MacroDef:
    params: list[str] | None
    body: str
    variadic: bool = False


@dataclass
class CondFrame:
    parent_active: bool
    any_taken: bool
    active: bool


def _is_ident_start(ch: str) -> bool:
    return ch.isalpha() or ch == "_"


def _is_ident_char(ch: str) -> bool:
    return ch.isalnum() or ch == "_"


def _resolve_include(
    include_name: str,
    source_path: str | None,
    include_dirs: list[str],
    system_include: bool = False,
) -> str | None:
    candidates: list[str] = []
    if source_path and not system_include:
        candidates.append(os.path.join(os.path.dirname(os.path.abspath(source_path)), include_name))
    for d in include_dirs:
        candidates.append(os.path.join(d, include_name))
    if not system_include:
        candidates.append(include_name)
    for p in candidates:
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


def _parse_macro_args(text: str, open_paren_idx: int) -> tuple[list[str], int] | None:
    if open_paren_idx >= len(text) or text[open_paren_idx] != "(":
        return None

    args: list[str] = []
    cur: list[str] = []
    i = open_paren_idx + 1
    depth = 1
    quote: str | None = None

    while i < len(text):
        ch = text[i]

        if quote is not None:
            cur.append(ch)
            if ch == "\\" and i + 1 < len(text):
                i += 1
                cur.append(text[i])
            elif ch == quote:
                quote = None
            i += 1
            continue

        if ch == '"' or ch == "'":
            quote = ch
            cur.append(ch)
            i += 1
            continue

        if ch == "(":
            depth += 1
            cur.append(ch)
            i += 1
            continue

        if ch == ")":
            depth -= 1
            if depth == 0:
                arg = "".join(cur).strip()
                if len(args) > 0 or arg != "":
                    args.append(arg)
                return args, i + 1
            cur.append(ch)
            i += 1
            continue

        if ch == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
            i += 1
            continue

        cur.append(ch)
        i += 1

    return None


def _replace_macro_params(body: str, param_map: dict[str, str]) -> str:
    out: list[str] = []
    i = 0
    n = len(body)
    quote: str | None = None

    while i < n:
        ch = body[i]

        if quote is not None:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(body[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = None
            i += 1
            continue

        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            continue

        if _is_ident_start(ch):
            j = i + 1
            while j < n and _is_ident_char(body[j]):
                j += 1
            tok = body[i:j]
            out.append(param_map.get(tok, tok))
            i = j
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def _stringize_text(text: str) -> str:
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    return f'"{escaped}"'


def _replace_stringize(body: str, raw_args: dict[str, str], var_raw: str) -> str:
    def repl(m: re.Match[str]) -> str:
        name = m.group(1)
        if name == "__VA_ARGS__":
            return _stringize_text(var_raw)
        if name in raw_args:
            return _stringize_text(raw_args[name])
        return m.group(0)

    return re.sub(r"(?<!#)#\s*([A-Za-z_][A-Za-z0-9_]*|__VA_ARGS__)", repl, body)


def _replace_token_paste(body: str, raw_args: dict[str, str], var_raw: str) -> str:
    def tok_value(tok: str) -> str:
        if tok == "__VA_ARGS__":
            return var_raw
        if tok in raw_args:
            return raw_args[tok]
        return tok

    out = body
    pat = re.compile(r"([A-Za-z_][A-Za-z0-9_]*|__VA_ARGS__)\s*##\s*([A-Za-z_][A-Za-z0-9_]*|__VA_ARGS__)")
    while True:
        changed = False

        def repl(m: re.Match[str]) -> str:
            nonlocal changed
            changed = True
            l = tok_value(m.group(1)).strip()
            r = tok_value(m.group(2)).strip()
            return l + r

        nxt = pat.sub(repl, out)
        out = nxt
        if not changed:
            break
    return out


def _expand_macro_body(
    macro: MacroDef,
    raw_args: dict[str, str],
    exp_args: dict[str, str],
    var_raw: str,
    var_exp: str,
) -> str:
    body = macro.body
    body = _replace_stringize(body, raw_args, var_raw)

    if macro.variadic:
        if var_exp == "":
            body = re.sub(r",\s*##\s*__VA_ARGS__", "", body)
        else:
            body = re.sub(r",\s*##\s*__VA_ARGS__", "," + var_exp, body)

    body = _replace_token_paste(body, raw_args, var_raw)
    body = _replace_macro_params(body, exp_args)
    body = body.replace("__VA_ARGS__", var_exp)
    return body


def _expand_macros_text(text: str, macros: dict[str, MacroDef], depth: int = 0, expanding: set[str] | None = None) -> str:
    if depth > 64:
        return text
    if expanding is None:
        expanding = set()

    out: list[str] = []
    i = 0
    n = len(text)
    quote: str | None = None

    while i < n:
        ch = text[i]

        if quote is not None:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = None
            i += 1
            continue

        if ch == '"' or ch == "'":
            quote = ch
            out.append(ch)
            i += 1
            continue

        if _is_ident_start(ch):
            j = i + 1
            while j < n and _is_ident_char(text[j]):
                j += 1
            name = text[i:j]
            macro = macros.get(name)
            if macro is None or name in expanding:
                out.append(name)
                i = j
                continue

            if macro.params is None:
                rep = _expand_macros_text(macro.body, macros, depth + 1, expanding | {name})
                out.append(rep)
                i = j
                continue

            k = j
            while k < n and text[k] in (" ", "\t"):
                k += 1
            parsed = _parse_macro_args(text, k)
            if parsed is None:
                out.append(name)
                i = j
                continue
            args_raw, end_idx = parsed

            fixed = len(macro.params)
            if macro.variadic:
                if len(args_raw) < fixed:
                    raise CompileError(1, 1, f"macro '{name}' expects at least {fixed} args, got {len(args_raw)}")
            else:
                if len(args_raw) != fixed:
                    raise CompileError(1, 1, f"macro '{name}' expects {fixed} args, got {len(args_raw)}")

            named_raw: dict[str, str] = {}
            named_exp: dict[str, str] = {}
            for idx, p in enumerate(macro.params):
                raw = args_raw[idx]
                named_raw[p] = raw
                named_exp[p] = _expand_macros_text(raw, macros, depth + 1, expanding)

            var_raw_args = args_raw[fixed:] if macro.variadic else []
            var_raw = ", ".join(var_raw_args)
            var_exp = ", ".join(_expand_macros_text(a, macros, depth + 1, expanding) for a in var_raw_args)

            body = _expand_macro_body(macro, named_raw, named_exp, var_raw, var_exp)
            rep = _expand_macros_text(body, macros, depth + 1, expanding | {name})
            out.append(rep)
            i = end_idx
            continue

        out.append(ch)
        i += 1

    expanded = "".join(out)
    if expanded != text and depth < 64:
        return _expand_macros_text(expanded, macros, depth + 1, expanding)
    return expanded


def _replace_defined(expr: str, macros: dict[str, MacroDef]) -> str:
    def repl_paren(m: re.Match[str]) -> str:
        name = m.group(1)
        return "1" if name in macros else "0"

    def repl_plain(m: re.Match[str]) -> str:
        name = m.group(1)
        return "1" if name in macros else "0"

    out = re.sub(r"\bdefined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", repl_paren, expr)
    out = re.sub(r"\bdefined\s+([A-Za-z_][A-Za-z0-9_]*)\b", repl_plain, out)
    return out


def _expand_condition_identifiers(expr: str, macros: dict[str, MacroDef], depth: int = 0) -> str:
    if depth > 32:
        return expr

    out: list[str] = []
    i = 0
    n = len(expr)
    changed = False

    while i < n:
        ch = expr[i]
        if _is_ident_start(ch):
            j = i + 1
            while j < n and _is_ident_char(expr[j]):
                j += 1
            name = expr[i:j]
            macro = macros.get(name)
            if macro is None:
                out.append("0")
                changed = True
            elif macro.params is None:
                rep = macro.body.strip()
                if rep == "":
                    rep = "1"
                out.append(rep)
                changed = True
            else:
                out.append("0")
                changed = True
            i = j
            continue
        out.append(ch)
        i += 1

    merged = "".join(out)
    if changed and merged != expr:
        return _expand_condition_identifiers(merged, macros, depth + 1)
    return merged


def _eval_if_expr(expr: str, macros: dict[str, MacroDef], line_no: int) -> bool:
    text = _replace_defined(expr, macros)
    text = _expand_condition_identifiers(text, macros)

    text = text.replace("&&", " and ")
    text = text.replace("||", " or ")
    text = re.sub(r"(?<![=!<>])!(?!=)", " not ", text)

    try:
        value = eval(text, {"__builtins__": {}}, {})
    except Exception as exc:
        raise CompileError(line_no, 1, f"invalid #if expression: {expr}") from exc
    return bool(value)


def _parse_define(rest: str, line_no: int) -> tuple[str, MacroDef]:
    if rest == "":
        raise CompileError(line_no, 1, "invalid #define")

    m = re.match(r"([A-Za-z_][A-Za-z0-9_]*)(.*)", rest)
    if not m:
        raise CompileError(line_no, 1, "invalid #define")

    name = m.group(1)
    tail = m.group(2)

    if tail.startswith("("):
        parsed = _parse_macro_args(tail, 0)
        if parsed is None:
            raise CompileError(line_no, 1, f"invalid function-like macro '{name}'")
        params_raw, end_idx = parsed

        variadic = False
        params: list[str] = []
        for i, p in enumerate(params_raw):
            if p == "...":
                if i != len(params_raw) - 1:
                    raise CompileError(line_no, 1, "'...' must be the last macro parameter")
                variadic = True
                continue
            if not _IDENT_RE.match(p):
                raise CompileError(line_no, 1, f"invalid macro parameter '{p}'")
            params.append(p)

        body = tail[end_idx:].lstrip()
        return name, MacroDef(params=params, body=body, variadic=variadic)

    body = tail.lstrip()
    if body == "":
        body = "1"
    return name, MacroDef(params=None, body=body, variadic=False)


def _preprocess(
    source: str,
    source_path: str | None,
    include_dirs: list[str],
    macros: dict[str, MacroDef],
    depth: int,
) -> str:
    if depth > 64:
        raise CompileError(1, 1, "preprocessor include depth exceeded")

    out_lines: list[str] = []
    cond_stack: list[CondFrame] = []

    lines = source.splitlines(keepends=True)
    for line_no, raw in enumerate(lines, start=1):
        stripped = raw.lstrip()
        active = cond_stack[-1].active if cond_stack else True

        if not stripped.startswith("#"):
            if active:
                out_lines.append(_expand_macros_text(raw, macros))
            continue

        directive_text = stripped[1:].strip()
        if directive_text == "":
            continue

        parts = directive_text.split(None, 1)
        name = parts[0]
        rest = parts[1] if len(parts) > 1 else ""

        if name == "if":
            parent = cond_stack[-1].active if cond_stack else True
            cond = _eval_if_expr(_expand_macros_text(rest, macros), macros, line_no) if parent else False
            cond_stack.append(CondFrame(parent_active=parent, any_taken=cond, active=(parent and cond)))
            continue

        if name == "ifdef":
            if not _IDENT_RE.match(rest):
                raise CompileError(line_no, 1, "invalid #ifdef")
            parent = cond_stack[-1].active if cond_stack else True
            cond = rest in macros
            cond_stack.append(CondFrame(parent_active=parent, any_taken=(parent and cond), active=(parent and cond)))
            continue

        if name == "ifndef":
            if not _IDENT_RE.match(rest):
                raise CompileError(line_no, 1, "invalid #ifndef")
            parent = cond_stack[-1].active if cond_stack else True
            cond = rest not in macros
            cond_stack.append(CondFrame(parent_active=parent, any_taken=(parent and cond), active=(parent and cond)))
            continue

        if name == "elif":
            if not cond_stack:
                raise CompileError(line_no, 1, "#elif without #if")
            frame = cond_stack[-1]
            if not frame.parent_active:
                frame.active = False
                continue
            if frame.any_taken:
                frame.active = False
                continue
            cond = _eval_if_expr(_expand_macros_text(rest, macros), macros, line_no)
            frame.active = cond
            if cond:
                frame.any_taken = True
            continue

        if name == "else":
            if not cond_stack:
                raise CompileError(line_no, 1, "#else without #if")
            frame = cond_stack[-1]
            if not frame.parent_active:
                frame.active = False
            else:
                frame.active = not frame.any_taken
                frame.any_taken = True
            continue

        if name == "endif":
            if not cond_stack:
                raise CompileError(line_no, 1, "#endif without #if")
            cond_stack.pop()
            continue

        if not active:
            continue

        if name == "define":
            macro_name, macro = _parse_define(rest, line_no)
            macros[macro_name] = macro
            continue

        if name == "undef":
            if not _IDENT_RE.match(rest):
                raise CompileError(line_no, 1, "invalid #undef")
            macros.pop(rest, None)
            continue

        if name == "include":
            inc_expr = _expand_macros_text(rest, macros).strip()
            system_include = False
            if len(inc_expr) >= 2 and inc_expr[0] == '"' and inc_expr[-1] == '"':
                inc_name = inc_expr[1:-1]
            elif len(inc_expr) >= 2 and inc_expr[0] == "<" and inc_expr[-1] == ">":
                inc_name = inc_expr[1:-1]
                system_include = True
            else:
                raise CompileError(line_no, 1, 'include must use "file" or <file>')

            inc_path = _resolve_include(inc_name, source_path, include_dirs, system_include=system_include)
            if not inc_path:
                raise CompileError(line_no, 1, f"include not found: {inc_name}")
            try:
                with open(inc_path, "r", encoding="utf-8", errors="replace") as f:
                    inc_text = f.read()
            except OSError as exc:
                raise CompileError(line_no, 1, f"include read failed: {inc_name}") from exc
            out_lines.append(_preprocess(inc_text, inc_path, include_dirs, macros, depth + 1))
            continue

        raise CompileError(line_no, 1, f"unsupported preprocessor directive: {name}")

    if cond_stack:
        raise CompileError(1, 1, "unterminated conditional directive")

    return "".join(out_lines)


def compile_source(source: str, source_path: str | None = None, include_dirs: list[str] | None = None) -> bytes:
    dirs = list(include_dirs) if include_dirs else []
    pre = _preprocess(source, source_path, dirs, {}, depth=0)
    toks = Lexer(pre).lex()
    ast = Parser(toks).parse_program()
    symbols = analyze(ast)
    result = generate(ast, symbols)
    return pack_native(result)
