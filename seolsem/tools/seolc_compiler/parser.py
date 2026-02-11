from __future__ import annotations

from dataclasses import dataclass

from .errors import CompileError
from .lexer import Token


@dataclass
class TypeSpec:
    base: str
    ptr: int
    struct_name: str | None
    line: int
    col: int


@dataclass
class Expr:
    line: int
    col: int


@dataclass
class Number(Expr):
    value: int


@dataclass
class StringLit(Expr):
    text: str


@dataclass
class Var(Expr):
    name: str


@dataclass
class Unary(Expr):
    op: str
    expr: Expr


@dataclass
class Binary(Expr):
    op: str
    lhs: Expr
    rhs: Expr


@dataclass
class Assign(Expr):
    lhs: Expr
    rhs: Expr


@dataclass
class Call(Expr):
    name: str
    args: list[Expr]

@dataclass
class Member(Expr):
    base: Expr
    field: str
    via_ptr: bool


@dataclass
class Index(Expr):
    base: Expr
    index: Expr


@dataclass
class Stmt:
    line: int
    col: int


@dataclass
class InitNode:
    line: int
    col: int


@dataclass
class InitExpr(InitNode):
    expr: Expr


@dataclass
class InitList(InitNode):
    items: list[InitNode]


@dataclass
class BlockStmt(Stmt):
    statements: list[Stmt]


@dataclass
class VarDecl(Stmt):
    type_spec: TypeSpec
    name: str
    array_dims: tuple[int, ...]
    init: InitNode | None


@dataclass
class ExprStmt(Stmt):
    expr: Expr | None


@dataclass
class ReturnStmt(Stmt):
    expr: Expr | None


@dataclass
class IfStmt(Stmt):
    cond: Expr
    then_branch: Stmt
    else_branch: Stmt | None


@dataclass
class WhileStmt(Stmt):
    cond: Expr
    body: Stmt


@dataclass
class ForStmt(Stmt):
    init: Stmt | Expr | None
    cond: Expr | None
    step: Expr | None
    body: Stmt


@dataclass
class BreakStmt(Stmt):
    pass


@dataclass
class ContinueStmt(Stmt):
    pass


@dataclass
class Param:
    type_spec: TypeSpec
    name: str
    line: int
    col: int


@dataclass
class StructDecl:
    name: str
    fields: list[Param]
    line: int
    col: int


@dataclass
class FunctionDef:
    ret_type: TypeSpec
    name: str
    params: list[Param]
    body: BlockStmt
    line: int
    col: int


@dataclass
class Program:
    structs: list[StructDecl]
    globals: list[VarDecl]
    functions: list[FunctionDef]


_PRECEDENCE = {
    "||": 1,
    "&&": 2,
    "|": 3,
    "^": 4,
    "&": 5,
    "==": 6,
    "!=": 6,
    "<": 7,
    "<=": 7,
    ">": 7,
    ">=": 7,
    "<<": 8,
    ">>": 8,
    "+": 9,
    "-": 9,
    "*": 10,
    "/": 10,
    "%": 10,
}


class Parser:
    def __init__(self, tokens: list[Token]):
        self.toks = tokens
        self.i = 0

    def _cur(self) -> Token:
        return self.toks[self.i]

    def _peek(self, offset: int = 1) -> Token:
        idx = self.i + offset
        if idx >= len(self.toks):
            return self.toks[-1]
        return self.toks[idx]

    def _adv(self) -> Token:
        tok = self._cur()
        if self.i < len(self.toks) - 1:
            self.i += 1
        return tok

    def _err(self, tok: Token, msg: str) -> CompileError:
        return CompileError(tok.line, tok.col, msg)

    def _is_symbol(self, sym: str) -> bool:
        tok = self._cur()
        return tok.kind == "SYMBOL" and tok.text == sym

    def _is_keyword(self, kw: str) -> bool:
        tok = self._cur()
        return tok.kind == "KEYWORD" and tok.text == kw

    def _expect_symbol(self, sym: str) -> Token:
        tok = self._cur()
        if tok.kind != "SYMBOL" or tok.text != sym:
            raise self._err(tok, f"expected '{sym}'")
        self._adv()
        return tok

    def _expect_keyword(self, kw: str) -> Token:
        tok = self._cur()
        if tok.kind != "KEYWORD" or tok.text != kw:
            raise self._err(tok, f"expected keyword '{kw}'")
        self._adv()
        return tok

    def _expect_ident(self) -> Token:
        tok = self._cur()
        if tok.kind != "IDENT":
            raise self._err(tok, "expected identifier")
        self._adv()
        return tok

    def _starts_type(self) -> bool:
        tok = self._cur()
        if tok.kind != "KEYWORD":
            return False
        if tok.text in ("int", "char", "void"):
            return True
        if tok.text == "struct":
            return True
        return False

    def parse_type(self) -> TypeSpec:
        tok = self._cur()
        if tok.kind != "KEYWORD":
            raise self._err(tok, "expected type")

        struct_name = None
        if tok.text == "struct":
            self._adv()
            name_tok = self._expect_ident()
            base = "struct"
            struct_name = name_tok.text
            line, col = tok.line, tok.col
        elif tok.text in ("int", "char", "void"):
            self._adv()
            base = tok.text
            line, col = tok.line, tok.col
        else:
            raise self._err(tok, "unsupported type")

        ptr = 0
        while self._is_symbol("*"):
            self._adv()
            ptr += 1

        return TypeSpec(base=base, ptr=ptr, struct_name=struct_name, line=line, col=col)

    def parse_struct_decl(self) -> StructDecl:
        kw = self._expect_keyword("struct")
        name_tok = self._expect_ident()
        self._expect_symbol("{")

        fields: list[Param] = []
        while not self._is_symbol("}"):
            t = self.parse_type()
            field_tok = self._expect_ident()
            self._expect_symbol(";")
            fields.append(Param(type_spec=t, name=field_tok.text, line=field_tok.line, col=field_tok.col))

        self._expect_symbol("}")
        self._expect_symbol(";")
        return StructDecl(name=name_tok.text, fields=fields, line=kw.line, col=kw.col)

    def parse_param_list(self) -> list[Param]:
        params: list[Param] = []

        if self._is_symbol(")"):
            return params

        if self._is_keyword("void") and self._peek().kind == "SYMBOL" and self._peek().text == ")":
            self._adv()
            return params

        while True:
            t = self.parse_type()
            name_tok = self._expect_ident()
            params.append(Param(type_spec=t, name=name_tok.text, line=name_tok.line, col=name_tok.col))
            if self._is_symbol(","):
                self._adv()
                continue
            break

        return params

    def parse_function(self) -> FunctionDef:
        ret = self.parse_type()
        name_tok = self._expect_ident()
        return self.parse_function_after_head(ret, name_tok)

    def parse_function_after_head(self, ret: TypeSpec, name_tok: Token) -> FunctionDef:
        self._expect_symbol("(")
        params = self.parse_param_list()
        self._expect_symbol(")")
        body = self.parse_block()
        return FunctionDef(ret_type=ret, name=name_tok.text, params=params, body=body, line=name_tok.line, col=name_tok.col)

    def parse_program(self) -> Program:
        structs: list[StructDecl] = []
        globals_: list[VarDecl] = []
        funcs: list[FunctionDef] = []

        while self._cur().kind != "EOF":
            if self._is_keyword("struct") and self._peek().kind == "IDENT" and self._peek(2).kind == "SYMBOL" and self._peek(2).text == "{":
                structs.append(self.parse_struct_decl())
                continue
            t = self.parse_type()
            name_tok = self._expect_ident()
            if self._is_symbol("("):
                funcs.append(self.parse_function_after_head(t, name_tok))
            else:
                globals_.append(self.parse_var_decl_after_head(t, name_tok, expect_semicolon=True))

        return Program(structs=structs, globals=globals_, functions=funcs)

    def parse_block(self) -> BlockStmt:
        tok = self._expect_symbol("{")
        out: list[Stmt] = []
        while not self._is_symbol("}"):
            out.append(self.parse_stmt())
        self._expect_symbol("}")
        return BlockStmt(line=tok.line, col=tok.col, statements=out)

    def parse_var_decl_after_head(self, t: TypeSpec, name_tok: Token, expect_semicolon: bool = True) -> VarDecl:
        dims: list[int] = []
        while self._is_symbol("["):
            self._adv()
            len_tok = self._cur()
            if len_tok.kind != "NUMBER":
                raise self._err(len_tok, "array size must be a constant number")
            self._adv()
            try:
                arr_len = int(len_tok.text, 0)
            except ValueError as exc:
                raise self._err(len_tok, "invalid array size") from exc
            if arr_len <= 0 or arr_len > 4096:
                raise self._err(len_tok, "array size out of range")
            dims.append(arr_len)
            self._expect_symbol("]")
        init = None
        if self._is_symbol("="):
            self._adv()
            init = self.parse_initializer()
        if expect_semicolon:
            self._expect_symbol(";")
        return VarDecl(
            line=name_tok.line,
            col=name_tok.col,
            type_spec=t,
            name=name_tok.text,
            array_dims=tuple(dims),
            init=init,
        )

    def parse_initializer(self) -> InitNode:
        tok = self._cur()
        if self._is_symbol("{"):
            self._adv()
            items: list[InitNode] = []
            if not self._is_symbol("}"):
                while True:
                    items.append(self.parse_initializer())
                    if self._is_symbol(","):
                        self._adv()
                        if self._is_symbol("}"):
                            break
                        continue
                    break
            self._expect_symbol("}")
            return InitList(line=tok.line, col=tok.col, items=items)

        expr = self.parse_expr()
        return InitExpr(line=expr.line, col=expr.col, expr=expr)

    def parse_var_decl_stmt(self, expect_semicolon: bool = True) -> VarDecl:
        t = self.parse_type()
        name_tok = self._expect_ident()
        return self.parse_var_decl_after_head(t, name_tok, expect_semicolon=expect_semicolon)

    def parse_stmt(self) -> Stmt:
        tok = self._cur()

        if self._is_symbol("{"):
            return self.parse_block()

        if self._is_keyword("if"):
            self._adv()
            self._expect_symbol("(")
            cond = self.parse_expr()
            self._expect_symbol(")")
            then_branch = self.parse_stmt()
            else_branch = None
            if self._is_keyword("else"):
                self._adv()
                else_branch = self.parse_stmt()
            return IfStmt(line=tok.line, col=tok.col, cond=cond, then_branch=then_branch, else_branch=else_branch)

        if self._is_keyword("while"):
            self._adv()
            self._expect_symbol("(")
            cond = self.parse_expr()
            self._expect_symbol(")")
            body = self.parse_stmt()
            return WhileStmt(line=tok.line, col=tok.col, cond=cond, body=body)

        if self._is_keyword("for"):
            self._adv()
            self._expect_symbol("(")

            init: Stmt | Expr | None
            if self._is_symbol(";"):
                init = None
                self._adv()
            elif self._starts_type():
                init = self.parse_var_decl_stmt(expect_semicolon=True)
            else:
                init = self.parse_expr()
                self._expect_symbol(";")

            cond = None
            if not self._is_symbol(";"):
                cond = self.parse_expr()
            self._expect_symbol(";")

            step = None
            if not self._is_symbol(")"):
                step = self.parse_expr()
            self._expect_symbol(")")

            body = self.parse_stmt()
            return ForStmt(line=tok.line, col=tok.col, init=init, cond=cond, step=step, body=body)

        if self._is_keyword("return"):
            self._adv()
            if self._is_symbol(";"):
                self._adv()
                return ReturnStmt(line=tok.line, col=tok.col, expr=None)
            expr = self.parse_expr()
            self._expect_symbol(";")
            return ReturnStmt(line=tok.line, col=tok.col, expr=expr)

        if self._is_keyword("break"):
            self._adv()
            self._expect_symbol(";")
            return BreakStmt(line=tok.line, col=tok.col)

        if self._is_keyword("continue"):
            self._adv()
            self._expect_symbol(";")
            return ContinueStmt(line=tok.line, col=tok.col)

        if self._starts_type():
            return self.parse_var_decl_stmt(expect_semicolon=True)

        if self._is_symbol(";"):
            self._adv()
            return ExprStmt(line=tok.line, col=tok.col, expr=None)

        expr = self.parse_expr()
        self._expect_symbol(";")
        return ExprStmt(line=tok.line, col=tok.col, expr=expr)

    def parse_expr(self) -> Expr:
        return self.parse_assign()

    def parse_assign(self) -> Expr:
        lhs = self.parse_binary(1)
        if self._is_symbol("="):
            tok = self._adv()
            rhs = self.parse_assign()
            return Assign(line=tok.line, col=tok.col, lhs=lhs, rhs=rhs)
        return lhs

    def parse_binary(self, min_prec: int) -> Expr:
        lhs = self.parse_unary()
        while True:
            tok = self._cur()
            if tok.kind != "SYMBOL" or tok.text not in _PRECEDENCE:
                break
            prec = _PRECEDENCE[tok.text]
            if prec < min_prec:
                break
            op = tok.text
            self._adv()
            rhs = self.parse_binary(prec + 1)
            lhs = Binary(line=tok.line, col=tok.col, op=op, lhs=lhs, rhs=rhs)
        return lhs

    def parse_unary(self) -> Expr:
        tok = self._cur()
        if tok.kind == "SYMBOL" and tok.text in ("-", "!", "*", "&", "+"):
            self._adv()
            expr = self.parse_unary()
            if tok.text == "+":
                return expr
            return Unary(line=tok.line, col=tok.col, op=tok.text, expr=expr)
        return self.parse_postfix()

    def parse_postfix(self) -> Expr:
        expr = self.parse_primary()
        while True:
            if self._is_symbol("("):
                open_tok = self._adv()
                args: list[Expr] = []
                if not self._is_symbol(")"):
                    while True:
                        args.append(self.parse_expr())
                        if self._is_symbol(","):
                            self._adv()
                            continue
                        break
                self._expect_symbol(")")

                if not isinstance(expr, Var):
                    raise self._err(open_tok, "unsupported call target")
                expr = Call(line=open_tok.line, col=open_tok.col, name=expr.name, args=args)
                continue

            tok = self._cur()
            if tok.kind == "SYMBOL" and tok.text in (".", "->"):
                self._adv()
                field_tok = self._expect_ident()
                expr = Member(
                    line=tok.line,
                    col=tok.col,
                    base=expr,
                    field=field_tok.text,
                    via_ptr=(tok.text == "->"),
                )
                continue

            if self._is_symbol("["):
                open_tok = self._adv()
                idx_expr = self.parse_expr()
                self._expect_symbol("]")
                expr = Index(line=open_tok.line, col=open_tok.col, base=expr, index=idx_expr)
                continue

            break
        return expr

    def parse_primary(self) -> Expr:
        tok = self._cur()

        if tok.kind == "NUMBER":
            self._adv()
            try:
                value = int(tok.text, 0)
            except ValueError as exc:
                raise self._err(tok, "invalid number") from exc
            if value < 0 or value > 0xFFFF:
                raise self._err(tok, "number out of 16-bit range")
            return Number(line=tok.line, col=tok.col, value=value)

        if tok.kind == "STRING":
            self._adv()
            return StringLit(line=tok.line, col=tok.col, text=tok.text)

        if tok.kind == "IDENT":
            self._adv()
            return Var(line=tok.line, col=tok.col, name=tok.text)

        if tok.kind == "SYMBOL" and tok.text == "(":
            self._adv()
            expr = self.parse_expr()
            self._expect_symbol(")")
            return expr

        raise self._err(tok, "expected expression")
