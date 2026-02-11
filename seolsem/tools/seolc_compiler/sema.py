from __future__ import annotations

from .errors import CompileError
from .parser import (
    Assign,
    Binary,
    BlockStmt,
    BreakStmt,
    Call,
    ContinueStmt,
    Expr,
    ExprStmt,
    ForStmt,
    FunctionDef,
    IfStmt,
    InitExpr,
    InitList,
    InitNode,
    Index,
    Member,
    Number,
    Program,
    ReturnStmt,
    StructDecl,
    StringLit,
    TypeSpec,
    Unary,
    Var,
    VarDecl,
    WhileStmt,
)


_IMPORTS = {
    "print",
    "print_num",
    "cls",
    "read_key",
    "write_char",
    "cursor_set",
    "cursor_show",
    "cursor_hide",
    "fs_write",
    "fs_read",
}


def _is_defined(name: str, scopes: list[set[str]]) -> bool:
    i = len(scopes) - 1
    while i >= 0:
        if name in scopes[i]:
            return True
        i -= 1
    return False


def _check_lvalue(expr: Expr) -> bool:
    if isinstance(expr, Var):
        return True
    if isinstance(expr, Unary) and expr.op == "*":
        return True
    if isinstance(expr, Member):
        return True
    if isinstance(expr, Index):
        return True
    return False

def _check_type_spec(t: TypeSpec, structs: dict[str, StructDecl]) -> None:
    if t.base == "struct":
        if not t.struct_name:
            raise CompileError(t.line, t.col, "struct type missing name")
        if t.struct_name not in structs:
            raise CompileError(t.line, t.col, f"unknown struct '{t.struct_name}'")
    if t.base == "void" and t.ptr == 0:
        return
    if t.base not in ("int", "char", "void", "struct"):
        raise CompileError(t.line, t.col, f"unsupported type '{t.base}'")


def _check_expr(expr: Expr, scopes: list[set[str]], funcs: set[str]) -> None:
    if isinstance(expr, Number):
        return
    if isinstance(expr, StringLit):
        return
    if isinstance(expr, Var):
        if not _is_defined(expr.name, scopes):
            raise CompileError(expr.line, expr.col, f"use of undefined variable '{expr.name}'")
        return
    if isinstance(expr, Unary):
        _check_expr(expr.expr, scopes, funcs)
        return
    if isinstance(expr, Member):
        _check_expr(expr.base, scopes, funcs)
        return
    if isinstance(expr, Index):
        _check_expr(expr.base, scopes, funcs)
        _check_expr(expr.index, scopes, funcs)
        return
    if isinstance(expr, Binary):
        _check_expr(expr.lhs, scopes, funcs)
        _check_expr(expr.rhs, scopes, funcs)
        return
    if isinstance(expr, Assign):
        if not _check_lvalue(expr.lhs):
            raise CompileError(expr.line, expr.col, "left side of assignment must be a variable or *pointer")
        _check_expr(expr.lhs, scopes, funcs)
        _check_expr(expr.rhs, scopes, funcs)
        return
    if isinstance(expr, Call):
        if expr.name not in funcs and expr.name not in _IMPORTS:
            raise CompileError(expr.line, expr.col, f"unknown function '{expr.name}'")
        for arg in expr.args:
            _check_expr(arg, scopes, funcs)
        return


def _check_initializer(init: InitNode, scopes: list[set[str]], funcs: set[str]) -> None:
    if isinstance(init, InitExpr):
        _check_expr(init.expr, scopes, funcs)
        return
    if isinstance(init, InitList):
        for item in init.items:
            _check_initializer(item, scopes, funcs)
        return


def _check_stmt(stmt, scopes: list[set[str]], funcs: set[str], loop_depth: int, structs: dict[str, StructDecl]) -> None:
    if isinstance(stmt, BlockStmt):
        scopes.append(set())
        for child in stmt.statements:
            _check_stmt(child, scopes, funcs, loop_depth, structs)
        scopes.pop()
        return

    if isinstance(stmt, VarDecl):
        cur = scopes[-1]
        _check_type_spec(stmt.type_spec, structs)
        if _is_defined(stmt.name, scopes):
            raise CompileError(stmt.line, stmt.col, f"duplicate declaration '{stmt.name}'")
        if len(stmt.array_dims) > 0 and stmt.type_spec.base == "void" and stmt.type_spec.ptr == 0:
            raise CompileError(stmt.line, stmt.col, "void array is not allowed")
        cur.add(stmt.name)
        if stmt.init is not None:
            if len(stmt.array_dims) > 0 and isinstance(stmt.init, InitExpr):
                raise CompileError(stmt.line, stmt.col, "array initializer must use braces")
            _check_initializer(stmt.init, scopes, funcs)
        return

    if isinstance(stmt, ExprStmt):
        if stmt.expr is not None:
            _check_expr(stmt.expr, scopes, funcs)
        return

    if isinstance(stmt, ReturnStmt):
        if stmt.expr is not None:
            _check_expr(stmt.expr, scopes, funcs)
        return

    if isinstance(stmt, IfStmt):
        _check_expr(stmt.cond, scopes, funcs)
        _check_stmt(stmt.then_branch, scopes, funcs, loop_depth, structs)
        if stmt.else_branch is not None:
            _check_stmt(stmt.else_branch, scopes, funcs, loop_depth, structs)
        return

    if isinstance(stmt, WhileStmt):
        _check_expr(stmt.cond, scopes, funcs)
        _check_stmt(stmt.body, scopes, funcs, loop_depth + 1, structs)
        return

    if isinstance(stmt, ForStmt):
        scopes.append(set())
        if stmt.init is not None:
            if isinstance(stmt.init, VarDecl):
                _check_stmt(stmt.init, scopes, funcs, loop_depth, structs)
            else:
                _check_expr(stmt.init, scopes, funcs)
        if stmt.cond is not None:
            _check_expr(stmt.cond, scopes, funcs)
        if stmt.step is not None:
            _check_expr(stmt.step, scopes, funcs)
        _check_stmt(stmt.body, scopes, funcs, loop_depth + 1, structs)
        scopes.pop()
        return

    if isinstance(stmt, BreakStmt):
        if loop_depth <= 0:
            raise CompileError(stmt.line, stmt.col, "'break' used outside loop")
        return

    if isinstance(stmt, ContinueStmt):
        if loop_depth <= 0:
            raise CompileError(stmt.line, stmt.col, "'continue' used outside loop")
        return


def _check_function(fn: FunctionDef, funcs: set[str], structs: dict[str, StructDecl]) -> None:
    scopes: list[set[str]] = [set()]
    _check_type_spec(fn.ret_type, structs)
    for p in fn.params:
        _check_type_spec(p.type_spec, structs)
        if p.type_spec.base == "void" and p.type_spec.ptr == 0:
            raise CompileError(p.line, p.col, "void parameter is not allowed")
        if p.name in scopes[0]:
            raise CompileError(p.line, p.col, f"duplicate parameter '{p.name}'")
        scopes[0].add(p.name)

    _check_stmt(fn.body, scopes, funcs, loop_depth=0, structs=structs)

def _check_structs(program: Program) -> dict[str, StructDecl]:
    structs: dict[str, StructDecl] = {}
    for st in program.structs:
        if st.name in structs:
            raise CompileError(st.line, st.col, f"duplicate struct '{st.name}'")
        structs[st.name] = st

    for st in program.structs:
        seen: set[str] = set()
        for f in st.fields:
            if f.name in seen:
                raise CompileError(f.line, f.col, f"duplicate field '{f.name}' in struct '{st.name}'")
            seen.add(f.name)
            _check_type_spec(f.type_spec, structs)
            if f.type_spec.base == "void" and f.type_spec.ptr == 0:
                raise CompileError(f.line, f.col, "void field is not allowed")
    return structs


def analyze(program: Program) -> dict[str, int]:
    funcs: dict[str, FunctionDef] = {}
    globals_set: set[str] = set()
    structs = _check_structs(program)

    for g in program.globals:
        _check_type_spec(g.type_spec, structs)
        if g.name in globals_set:
            raise CompileError(g.line, g.col, f"duplicate global '{g.name}'")
        if g.name in funcs:
            raise CompileError(g.line, g.col, f"duplicate symbol '{g.name}'")
        if len(g.array_dims) > 0 and g.type_spec.base == "void" and g.type_spec.ptr == 0:
            raise CompileError(g.line, g.col, "void array is not allowed")
        globals_set.add(g.name)

    for fn in program.functions:
        if fn.name in funcs:
            raise CompileError(fn.line, fn.col, f"duplicate function '{fn.name}'")
        if fn.name in globals_set:
            raise CompileError(fn.line, fn.col, f"duplicate symbol '{fn.name}'")
        funcs[fn.name] = fn

    if "main" not in funcs:
        raise CompileError(1, 1, "missing main()")

    func_names = set(funcs.keys())
    global_scopes: list[set[str]] = [set(globals_set)]
    for g in program.globals:
        if g.init is not None:
            if len(g.array_dims) > 0 and isinstance(g.init, InitExpr):
                raise CompileError(g.line, g.col, "array initializer must use braces")
            _check_initializer(g.init, global_scopes, func_names)

    for fn in program.functions:
        scopes_seed = set(globals_set)
        # shadowing globals is currently disallowed for deterministic codegen.
        scopes: list[set[str]] = [scopes_seed]
        _check_type_spec(fn.ret_type, structs)
        for p in fn.params:
            _check_type_spec(p.type_spec, structs)
            if p.type_spec.base == "void" and p.type_spec.ptr == 0:
                raise CompileError(p.line, p.col, "void parameter is not allowed")
            if p.name in scopes[0]:
                raise CompileError(p.line, p.col, f"duplicate parameter '{p.name}'")
            scopes[0].add(p.name)
        _check_stmt(fn.body, scopes, func_names, loop_depth=0, structs=structs)

    return {name: idx for idx, name in enumerate(funcs.keys())}
