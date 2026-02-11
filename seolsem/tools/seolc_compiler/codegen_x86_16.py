from __future__ import annotations

from dataclasses import dataclass

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
    Param,
    Program,
    ReturnStmt,
    StringLit,
    StructDecl,
    TypeSpec,
    Unary,
    Var,
    VarDecl,
    WhileStmt,
)


RELOC_BASE16 = 0x01
RELOC_IMPORT16 = 0x02


@dataclass
class RelocEntry:
    patch_off: int
    reloc_type: int
    arg: int


@dataclass
class CodegenResult:
    entry: int
    code: bytes
    data: bytes
    bss_size: int
    imports: list[str]
    relocs: list[RelocEntry]


@dataclass
class CType:
    base: str
    ptr: int
    struct_name: str | None
    array_dims: tuple[int, ...] = ()


@dataclass
class StructField:
    ctype: CType
    offset: int
    size: int


@dataclass
class StructInfo:
    name: str
    size: int
    fields: dict[str, StructField]


@dataclass
class VarSlot:
    is_param: bool
    is_global: bool
    offset: int
    size: int
    ctype: CType
    array_dims: tuple[int, ...]


class CodeBuilder:
    def __init__(self) -> None:
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.rel_fixups: list[tuple[int, str]] = []
        self.relocs: list[RelocEntry] = []

        self.imports: list[str] = []
        self.import_map: dict[str, int] = {}

        self.data = bytearray()
        self.string_off: dict[str, int] = {}
        self.data_fixups: list[tuple[int, str]] = []
        self.global_fixups: list[tuple[int, int]] = []

        self._label_seq = 0

    def new_label(self, prefix: str) -> str:
        self._label_seq += 1
        return f"{prefix}_{self._label_seq}"

    def get_import_index(self, name: str, line: int, col: int) -> int:
        if name in self.import_map:
            return self.import_map[name]
        idx = len(self.imports)
        if idx > 255:
            raise CompileError(line, col, "too many imports")
        self.import_map[name] = idx
        self.imports.append(name)
        return idx

    def intern_string(self, text: str) -> int:
        if text in self.string_off:
            return self.string_off[text]
        off = len(self.data)
        self.data.extend(text.encode("ascii", errors="replace"))
        self.data.append(0)
        self.string_off[text] = off
        return off

    def emit_u8(self, value: int) -> None:
        self.code.append(value & 0xFF)

    def emit_u16(self, value: int) -> int:
        pos = len(self.code)
        self.code.append(value & 0xFF)
        self.code.append((value >> 8) & 0xFF)
        return pos

    def patch_u16(self, pos: int, value: int) -> None:
        self.code[pos] = value & 0xFF
        self.code[pos + 1] = (value >> 8) & 0xFF

    def mark_label(self, name: str) -> None:
        self.labels[name] = len(self.code)

    def emit_jmp_label(self, label: str) -> None:
        self.emit_u8(0xE9)
        pos = self.emit_u16(0)
        self.rel_fixups.append((pos, label))

    def emit_call_label(self, label: str) -> None:
        self.emit_u8(0xE8)
        pos = self.emit_u16(0)
        self.rel_fixups.append((pos, label))

    def resolve_rel_fixups(self) -> None:
        for pos, label in self.rel_fixups:
            if label not in self.labels:
                raise CompileError(1, 1, f"internal label not found: {label}")
            target = self.labels[label]
            disp = target - (pos + 2)
            if disp < -32768 or disp > 32767:
                raise CompileError(1, 1, "jump/call out of range")
            self.patch_u16(pos, disp & 0xFFFF)

    def finalize(self, entry_label: str, bss_size: int) -> CodegenResult:
        if entry_label not in self.labels:
            raise CompileError(1, 1, "entry label not emitted")

        self.resolve_rel_fixups()

        code_size = len(self.code)
        for patch_pos, text in self.data_fixups:
            data_off = self.string_off[text]
            self.patch_u16(patch_pos, code_size + data_off)
            self.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_BASE16, arg=0))
        data_size = len(self.data)
        for patch_pos, bss_off in self.global_fixups:
            self.patch_u16(patch_pos, code_size + data_size + bss_off)
            self.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_BASE16, arg=0))

        return CodegenResult(
            entry=self.labels[entry_label],
            code=bytes(self.code),
            data=bytes(self.data),
            bss_size=bss_size,
            imports=list(self.imports),
            relocs=list(self.relocs),
        )


class FunctionEnv:
    def __init__(
        self,
        builder: CodeBuilder,
        fn: FunctionDef,
        internal_funcs: set[str],
        struct_infos: dict[str, StructInfo],
        global_vars: dict[str, VarSlot],
    ):
        self.b = builder
        self.fn = fn
        self.internal_funcs = internal_funcs
        self.struct_infos = struct_infos
        self.vars: dict[str, VarSlot] = dict(global_vars)
        self.break_labels: list[str] = []
        self.continue_labels: list[str] = []
        self.epilogue_label = self.b.new_label(f"{fn.name}_epilogue")

    def find_var(self, name: str, line: int, col: int) -> VarSlot:
        slot = self.vars.get(name)
        if slot is None:
            raise CompileError(line, col, f"unknown variable '{name}'")
        return slot


def _type_from_spec(t: TypeSpec) -> CType:
    return CType(base=t.base, ptr=t.ptr, struct_name=t.struct_name, array_dims=())


def _type_is_struct_value(t: CType) -> bool:
    return t.base == "struct" and t.ptr == 0 and len(t.array_dims) == 0


def _type_is_char_scalar(t: CType) -> bool:
    return t.base == "char" and t.ptr == 0 and len(t.array_dims) == 0


def _type_size(t: CType, struct_infos: dict[str, StructInfo], line: int, col: int) -> int:
    if t.ptr > 0:
        return 2
    if t.base == "char":
        base_size = 1
    elif t.base in ("int", "void"):
        base_size = 2
    elif t.base == "struct":
        if not t.struct_name or t.struct_name not in struct_infos:
            raise CompileError(line, col, f"unknown struct '{t.struct_name}'")
        base_size = struct_infos[t.struct_name].size
    else:
        raise CompileError(line, col, f"unsupported type '{t.base}'")
    size = base_size
    for n in t.array_dims:
        size *= n
    return size


def _build_struct_layouts(program: Program) -> dict[str, StructInfo]:
    decl_map: dict[str, StructDecl] = {s.name: s for s in program.structs}
    info_map: dict[str, StructInfo] = {}
    visiting: set[str] = set()

    def ensure(name: str) -> StructInfo:
        if name in info_map:
            return info_map[name]
        if name in visiting:
            raise CompileError(1, 1, f"recursive struct value dependency for '{name}'")
        if name not in decl_map:
            raise CompileError(1, 1, f"unknown struct '{name}'")

        visiting.add(name)
        decl = decl_map[name]

        offset = 0
        fields: dict[str, StructField] = {}
        for f in decl.fields:
            ct = _type_from_spec(f.type_spec)
            if _type_is_struct_value(ct):
                if not ct.struct_name:
                    raise CompileError(f.line, f.col, "invalid struct field type")
                sub = ensure(ct.struct_name)
                size = sub.size
            else:
                size = _type_size(ct, info_map, f.line, f.col)
            fields[f.name] = StructField(ctype=ct, offset=offset, size=size)
            offset += size

        if offset <= 0:
            offset = 2

        info = StructInfo(name=name, size=offset, fields=fields)
        info_map[name] = info
        visiting.remove(name)
        return info

    for name in decl_map:
        ensure(name)

    return info_map


def _emit_mov_ax_imm(b: CodeBuilder, value: int) -> int:
    b.emit_u8(0xB8)
    return b.emit_u16(value)


def _emit_mov_bx_imm(b: CodeBuilder, value: int) -> int:
    b.emit_u8(0xBB)
    return b.emit_u16(value)


def _emit_mov_cx_imm(b: CodeBuilder, value: int) -> int:
    b.emit_u8(0xB9)
    return b.emit_u16(value)


def _emit_mov_bx_ax(b: CodeBuilder) -> None:
    b.emit_u8(0x89)
    b.emit_u8(0xC3)


def _emit_load_u8_ax_from_bx(b: CodeBuilder) -> None:
    # mov al, [bx]; mov ah, 0
    b.emit_u8(0x8A)
    b.emit_u8(0x07)
    b.emit_u8(0xB4)
    b.emit_u8(0x00)


def _emit_store_u8_al_to_bx(b: CodeBuilder) -> None:
    # mov [bx], al
    b.emit_u8(0x88)
    b.emit_u8(0x07)


def _emit_push_ax(b: CodeBuilder) -> None:
    b.emit_u8(0x50)


def _emit_pop_bx(b: CodeBuilder) -> None:
    b.emit_u8(0x5B)


def _emit_add_sp_imm(b: CodeBuilder, value: int) -> None:
    if value <= 0:
        return
    if value <= 127:
        b.emit_u8(0x83)
        b.emit_u8(0xC4)
        b.emit_u8(value)
    else:
        b.emit_u8(0x81)
        b.emit_u8(0xC4)
        b.emit_u16(value)


def _emit_add_ax_imm(b: CodeBuilder, value: int) -> None:
    if value == 0:
        return
    b.emit_u8(0x05)
    b.emit_u16(value & 0xFFFF)


def _emit_mov_ax_bp_disp(b: CodeBuilder, disp: int) -> None:
    if -128 <= disp <= 127:
        b.emit_u8(0x8B)
        b.emit_u8(0x46)
        b.emit_u8(disp & 0xFF)
    else:
        b.emit_u8(0x8B)
        b.emit_u8(0x86)
        b.emit_u16(disp & 0xFFFF)


def _emit_mov_bp_disp_ax(b: CodeBuilder, disp: int) -> None:
    if -128 <= disp <= 127:
        b.emit_u8(0x89)
        b.emit_u8(0x46)
        b.emit_u8(disp & 0xFF)
    else:
        b.emit_u8(0x89)
        b.emit_u8(0x86)
        b.emit_u16(disp & 0xFFFF)


def _emit_lea_ax_bp_disp(b: CodeBuilder, disp: int) -> None:
    if -128 <= disp <= 127:
        b.emit_u8(0x8D)
        b.emit_u8(0x46)
        b.emit_u8(disp & 0xFF)
    else:
        b.emit_u8(0x8D)
        b.emit_u8(0x86)
        b.emit_u16(disp & 0xFFFF)


def _emit_cmp_ax_zero(b: CodeBuilder) -> None:
    b.emit_u8(0x83)
    b.emit_u8(0xF8)
    b.emit_u8(0x00)


def _emit_cmp_bx_ax(b: CodeBuilder) -> None:
    b.emit_u8(0x39)
    b.emit_u8(0xC3)


def _emit_set_ax_bool_from_jcc(b: CodeBuilder, jcc_opcode: int) -> None:
    _emit_mov_ax_imm(b, 0)
    b.emit_u8(jcc_opcode)
    # Jump over the 2-byte short jmp to land exactly at "mov ax,1".
    b.emit_u8(0x02)
    b.emit_u8(0xEB)
    b.emit_u8(0x03)
    _emit_mov_ax_imm(b, 1)


def _emit_jump_if_ax_zero(b: CodeBuilder, false_label: str) -> None:
    _emit_cmp_ax_zero(b)
    b.emit_u8(0x75)
    b.emit_u8(0x03)
    b.emit_jmp_label(false_label)


def _emit_jump_if_ax_nonzero(b: CodeBuilder, true_label: str) -> None:
    _emit_cmp_ax_zero(b)
    b.emit_u8(0x74)
    b.emit_u8(0x03)
    b.emit_jmp_label(true_label)


def _collect_locals_stmt(stmt, decls: dict[str, VarDecl]) -> None:
    if isinstance(stmt, BlockStmt):
        for s in stmt.statements:
            _collect_locals_stmt(s, decls)
        return

    if isinstance(stmt, VarDecl):
        if stmt.name not in decls:
            decls[stmt.name] = stmt
        return

    if isinstance(stmt, IfStmt):
        _collect_locals_stmt(stmt.then_branch, decls)
        if stmt.else_branch is not None:
            _collect_locals_stmt(stmt.else_branch, decls)
        return

    if isinstance(stmt, WhileStmt):
        _collect_locals_stmt(stmt.body, decls)
        return

    if isinstance(stmt, ForStmt):
        if isinstance(stmt.init, VarDecl):
            _collect_locals_stmt(stmt.init, decls)
        _collect_locals_stmt(stmt.body, decls)
        return


def _prepare_var_slots(env: FunctionEnv) -> int:
    i = 0
    for p in env.fn.params:
        env.vars[p.name] = VarSlot(
            is_param=True,
            is_global=False,
            offset=4 + i * 2,
            size=2,
            ctype=_type_from_spec(p.type_spec),
            array_dims=(),
        )
        i += 1

    local_decls: dict[str, VarDecl] = {}
    _collect_locals_stmt(env.fn.body, local_decls)

    used = 0
    for name, decl in local_decls.items():
        ct = _type_from_spec(decl.type_spec)
        ct = CType(base=ct.base, ptr=ct.ptr, struct_name=ct.struct_name, array_dims=decl.array_dims)
        size = _type_size(ct, env.struct_infos, decl.type_spec.line, decl.type_spec.col)
        if size <= 0:
            size = 2
        used += size
        env.vars[name] = VarSlot(
            is_param=False,
            is_global=False,
            offset=-used,
            size=size,
            ctype=ct,
            array_dims=decl.array_dims,
        )

    return used


def _expr_type(env: FunctionEnv, expr: Expr) -> CType:
    if isinstance(expr, Number):
        return CType(base="int", ptr=0, struct_name=None)
    if isinstance(expr, StringLit):
        return CType(base="char", ptr=1, struct_name=None)
    if isinstance(expr, Var):
        slot = env.find_var(expr.name, expr.line, expr.col)
        if len(slot.array_dims) > 0:
            return CType(
                base=slot.ctype.base,
                ptr=slot.ctype.ptr + 1,
                struct_name=slot.ctype.struct_name,
                array_dims=slot.array_dims[1:],
            )
        return slot.ctype
    if isinstance(expr, Unary):
        if expr.op == "&":
            inner = _expr_type(env, expr.expr)
            return CType(base=inner.base, ptr=inner.ptr + 1, struct_name=inner.struct_name, array_dims=inner.array_dims)
        if expr.op == "*":
            inner = _expr_type(env, expr.expr)
            if inner.ptr <= 0:
                raise CompileError(expr.line, expr.col, "cannot dereference non-pointer")
            return CType(base=inner.base, ptr=inner.ptr - 1, struct_name=inner.struct_name, array_dims=inner.array_dims)
        return CType(base="int", ptr=0, struct_name=None)
    if isinstance(expr, Binary):
        if expr.op in ("==", "!=", "<", "<=", ">", ">=", "&&", "||"):
            return CType(base="int", ptr=0, struct_name=None)
        return _expr_type(env, expr.lhs)
    if isinstance(expr, Assign):
        return _expr_type(env, expr.lhs)
    if isinstance(expr, Call):
        return CType(base="int", ptr=0, struct_name=None)
    if isinstance(expr, Member):
        _, ft = _member_resolve(env, expr)
        return ft
    if isinstance(expr, Index):
        et = _index_elem_type(env, expr)
        if et.ptr == 0 and len(et.array_dims) > 0:
            return CType(base=et.base, ptr=1, struct_name=et.struct_name, array_dims=et.array_dims[1:])
        return et
    return CType(base="int", ptr=0, struct_name=None)


def _member_resolve(env: FunctionEnv, expr: Member) -> tuple[int, CType]:
    bt = _expr_type(env, expr.base)

    if expr.via_ptr:
        if bt.ptr <= 0:
            raise CompileError(expr.line, expr.col, "'->' requires pointer base")
        bt = CType(base=bt.base, ptr=bt.ptr - 1, struct_name=bt.struct_name)

    if bt.base != "struct" or bt.ptr != 0 or not bt.struct_name:
        raise CompileError(expr.line, expr.col, "member access requires struct value")

    info = env.struct_infos.get(bt.struct_name)
    if not info:
        raise CompileError(expr.line, expr.col, f"unknown struct '{bt.struct_name}'")

    field = info.fields.get(expr.field)
    if not field:
        raise CompileError(expr.line, expr.col, f"struct '{bt.struct_name}' has no field '{expr.field}'")

    return field.offset, field.ctype


def _index_elem_type(env: FunctionEnv, expr: Index) -> CType:
    bt = _expr_type(env, expr.base)
    if bt.ptr <= 0:
        raise CompileError(expr.line, expr.col, "subscript requires pointer/array base")
    return CType(base=bt.base, ptr=bt.ptr - 1, struct_name=bt.struct_name, array_dims=bt.array_dims)


def _lvalue_type(env: FunctionEnv, expr: Expr) -> CType:
    if isinstance(expr, Var):
        slot = env.find_var(expr.name, expr.line, expr.col)
        if len(slot.array_dims) > 0:
            raise CompileError(expr.line, expr.col, "array is not assignable")
        return slot.ctype
    if isinstance(expr, Unary) and expr.op == "*":
        t = _expr_type(env, expr.expr)
        if t.ptr <= 0:
            raise CompileError(expr.line, expr.col, "cannot dereference non-pointer")
        return CType(base=t.base, ptr=t.ptr - 1, struct_name=t.struct_name, array_dims=t.array_dims)
    if isinstance(expr, Member):
        _, t = _member_resolve(env, expr)
        return t
    if isinstance(expr, Index):
        return _index_elem_type(env, expr)
    raise CompileError(expr.line, expr.col, "left side of assignment must be lvalue")


def _compile_struct_copy(env: FunctionEnv, dst: Expr, src: Expr, byte_size: int) -> None:
    b = env.b
    words = byte_size // 2
    tail = byte_size % 2

    _compile_lvalue_address(env, dst)
    _emit_push_ax(b)
    _compile_lvalue_address(env, src)
    b.emit_u8(0x89)  # mov si, ax
    b.emit_u8(0xC6)
    b.emit_u8(0x5F)  # pop di

    if words > 0:
        _emit_mov_cx_imm(b, words)
        b.emit_u8(0xFC)  # cld
        b.emit_u8(0xF3)  # rep movsw
        b.emit_u8(0xA5)

    if tail > 0:
        b.emit_u8(0xFC)  # cld
        b.emit_u8(0xA4)  # movsb

    _emit_mov_ax_imm(b, 0)


def _compile_lvalue_address(env: FunctionEnv, expr: Expr) -> None:
    b = env.b

    if isinstance(expr, Var):
        slot = env.find_var(expr.name, expr.line, expr.col)
        if slot.is_global:
            patch_pos = _emit_mov_ax_imm(b, 0)
            b.global_fixups.append((patch_pos, slot.offset))
        else:
            _emit_lea_ax_bp_disp(b, slot.offset)
        return

    if isinstance(expr, Unary) and expr.op == "*":
        _compile_expr(env, expr.expr)
        return

    if isinstance(expr, Member):
        field_off, _ = _member_resolve(env, expr)
        if expr.via_ptr:
            _compile_expr(env, expr.base)
        else:
            _compile_lvalue_address(env, expr.base)
        _emit_add_ax_imm(b, field_off)
        return

    if isinstance(expr, Index):
        elem_type = _index_elem_type(env, expr)
        elem_size = _type_size(elem_type, env.struct_infos, expr.line, expr.col)

        _compile_expr(env, expr.base)
        _emit_push_ax(b)
        _compile_expr(env, expr.index)

        if elem_size > 1:
            _emit_mov_cx_imm(b, elem_size)
            b.emit_u8(0xF7)  # mul cx
            b.emit_u8(0xE1)

        _emit_pop_bx(b)
        b.emit_u8(0x01)  # add ax, bx
        b.emit_u8(0xD8)
        return

    raise CompileError(expr.line, expr.col, "not an lvalue")


def _compile_expr(env: FunctionEnv, expr: Expr) -> None:
    b = env.b

    if isinstance(expr, Number):
        _emit_mov_ax_imm(b, expr.value)
        return

    if isinstance(expr, StringLit):
        b.intern_string(expr.text)
        patch_pos = _emit_mov_ax_imm(b, 0)
        b.data_fixups.append((patch_pos, expr.text))
        return

    if isinstance(expr, Var):
        slot = env.find_var(expr.name, expr.line, expr.col)
        if len(slot.array_dims) > 0:
            _compile_lvalue_address(env, expr)
            return
        if _type_is_struct_value(slot.ctype):
            _compile_lvalue_address(env, expr)
            return
        _compile_lvalue_address(env, expr)
        _emit_mov_bx_ax(b)
        if _type_is_char_scalar(slot.ctype):
            _emit_load_u8_ax_from_bx(b)
        else:
            b.emit_u8(0x8B)
            b.emit_u8(0x07)
        return

    if isinstance(expr, Member):
        _, ft = _member_resolve(env, expr)
        if _type_is_struct_value(ft):
            _compile_lvalue_address(env, expr)
            return
        _compile_lvalue_address(env, expr)
        _emit_mov_bx_ax(b)
        if _type_is_char_scalar(ft):
            _emit_load_u8_ax_from_bx(b)
        else:
            b.emit_u8(0x8B)
            b.emit_u8(0x07)
        return

    if isinstance(expr, Index):
        et = _index_elem_type(env, expr)
        if _type_is_struct_value(et):
            _compile_lvalue_address(env, expr)
            return
        _compile_lvalue_address(env, expr)
        _emit_mov_bx_ax(b)
        if _type_is_char_scalar(et):
            _emit_load_u8_ax_from_bx(b)
        else:
            b.emit_u8(0x8B)  # mov ax, [bx]
            b.emit_u8(0x07)
        return

    if isinstance(expr, Unary):
        if expr.op == "&":
            _compile_lvalue_address(env, expr.expr)
            return

        _compile_expr(env, expr.expr)

        if expr.op == "-":
            b.emit_u8(0xF7)
            b.emit_u8(0xD8)
            return

        if expr.op == "!":
            _emit_cmp_ax_zero(b)
            _emit_set_ax_bool_from_jcc(b, 0x74)
            return

        if expr.op == "*":
            deref_t = _expr_type(env, expr)
            _emit_mov_bx_ax(b)
            if _type_is_struct_value(deref_t):
                return
            if _type_is_char_scalar(deref_t):
                _emit_load_u8_ax_from_bx(b)
            else:
                b.emit_u8(0x8B)
                b.emit_u8(0x07)
            return

        raise CompileError(expr.line, expr.col, f"unsupported unary operator '{expr.op}'")

    if isinstance(expr, Assign):
        lt = _lvalue_type(env, expr.lhs)
        if _type_is_struct_value(lt):
            rt = _lvalue_type(env, expr.rhs)
            if (
                lt.base != rt.base
                or lt.ptr != rt.ptr
                or lt.struct_name != rt.struct_name
                or lt.array_dims != rt.array_dims
            ):
                raise CompileError(expr.line, expr.col, "struct assignment type mismatch")
            _compile_struct_copy(env, expr.lhs, expr.rhs, _type_size(lt, env.struct_infos, expr.line, expr.col))
            return

        rt = _expr_type(env, expr.rhs)
        if _type_is_struct_value(rt):
            raise CompileError(expr.line, expr.col, "cannot assign struct value to scalar target")

        _compile_lvalue_address(env, expr.lhs)
        _emit_push_ax(b)
        _compile_expr(env, expr.rhs)
        _emit_pop_bx(b)
        if _type_is_char_scalar(lt):
            _emit_store_u8_al_to_bx(b)
        else:
            b.emit_u8(0x89)
            b.emit_u8(0x07)
        return

    if isinstance(expr, Binary):
        lt = _expr_type(env, expr.lhs)
        rt = _expr_type(env, expr.rhs)
        if _type_is_struct_value(lt) or _type_is_struct_value(rt):
            raise CompileError(expr.line, expr.col, "struct value is not valid in binary expression")

        if expr.op == "&&":
            false_label = b.new_label("land_false")
            end_label = b.new_label("land_end")
            _compile_expr(env, expr.lhs)
            _emit_jump_if_ax_zero(b, false_label)
            _compile_expr(env, expr.rhs)
            _emit_jump_if_ax_zero(b, false_label)
            _emit_mov_ax_imm(b, 1)
            b.emit_jmp_label(end_label)
            b.mark_label(false_label)
            _emit_mov_ax_imm(b, 0)
            b.mark_label(end_label)
            return

        if expr.op == "||":
            true_label = b.new_label("lor_true")
            end_label = b.new_label("lor_end")
            _compile_expr(env, expr.lhs)
            _emit_jump_if_ax_nonzero(b, true_label)
            _compile_expr(env, expr.rhs)
            _emit_jump_if_ax_nonzero(b, true_label)
            _emit_mov_ax_imm(b, 0)
            b.emit_jmp_label(end_label)
            b.mark_label(true_label)
            _emit_mov_ax_imm(b, 1)
            b.mark_label(end_label)
            return

        _compile_expr(env, expr.lhs)
        _emit_push_ax(b)
        _compile_expr(env, expr.rhs)
        _emit_pop_bx(b)

        if expr.op == "+":
            b.emit_u8(0x01)
            b.emit_u8(0xC3)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            return

        if expr.op == "-":
            b.emit_u8(0x29)
            b.emit_u8(0xC3)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            return

        if expr.op == "*":
            b.emit_u8(0x89)
            b.emit_u8(0xC1)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            b.emit_u8(0xF7)
            b.emit_u8(0xE9)
            return

        if expr.op == "/":
            b.emit_u8(0x89)
            b.emit_u8(0xC1)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            b.emit_u8(0x99)
            b.emit_u8(0xF7)
            b.emit_u8(0xF9)
            return

        if expr.op == "%":
            b.emit_u8(0x89)
            b.emit_u8(0xC1)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            b.emit_u8(0x99)
            b.emit_u8(0xF7)
            b.emit_u8(0xF9)
            b.emit_u8(0x89)
            b.emit_u8(0xD0)
            return

        if expr.op == "&":
            b.emit_u8(0x21)
            b.emit_u8(0xC3)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            return

        if expr.op == "|":
            b.emit_u8(0x09)
            b.emit_u8(0xC3)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            return

        if expr.op == "^":
            b.emit_u8(0x31)
            b.emit_u8(0xC3)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            return

        if expr.op == "<<":
            b.emit_u8(0x89)
            b.emit_u8(0xC1)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            b.emit_u8(0xD3)
            b.emit_u8(0xE0)
            return

        if expr.op == ">>":
            b.emit_u8(0x89)
            b.emit_u8(0xC1)
            b.emit_u8(0x89)
            b.emit_u8(0xD8)
            b.emit_u8(0xD3)
            b.emit_u8(0xF8)
            return

        if expr.op == "==":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x74)
            return

        if expr.op == "!=":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x75)
            return

        if expr.op == "<":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x7C)
            return

        if expr.op == "<=":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x7E)
            return

        if expr.op == ">":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x7F)
            return

        if expr.op == ">=":
            _emit_cmp_bx_ax(b)
            _emit_set_ax_bool_from_jcc(b, 0x7D)
            return

        raise CompileError(expr.line, expr.col, f"unsupported binary operator '{expr.op}'")

    if isinstance(expr, Call):
        name = expr.name

        if name == "print":
            if len(expr.args) == 0:
                empty = StringLit(line=expr.line, col=expr.col, text="")
                _compile_expr(env, empty)
                _emit_push_ax(b)
                idx = b.get_import_index("print", expr.line, expr.col)
                patch_pos = _emit_mov_bx_imm(b, 0)
                b.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_IMPORT16, arg=idx))
                b.emit_u8(0xFF)
                b.emit_u8(0xD3)
                _emit_add_sp_imm(b, 2)
                _emit_mov_ax_imm(b, 0)
                return

            if len(expr.args) == 1 and isinstance(expr.args[0], StringLit):
                _compile_expr(env, expr.args[0])
                _emit_push_ax(b)
                idx = b.get_import_index("print", expr.line, expr.col)
                patch_pos = _emit_mov_bx_imm(b, 0)
                b.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_IMPORT16, arg=idx))
                b.emit_u8(0xFF)
                b.emit_u8(0xD3)
                _emit_add_sp_imm(b, 2)
                _emit_mov_ax_imm(b, 0)
                return

            if len(expr.args) != 1:
                raise CompileError(expr.line, expr.col, "print() supports exactly one argument")

            _compile_expr(env, expr.args[0])
            _emit_push_ax(b)
            idx = b.get_import_index("print_num", expr.line, expr.col)
            patch_pos = _emit_mov_bx_imm(b, 0)
            b.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_IMPORT16, arg=idx))
            b.emit_u8(0xFF)
            b.emit_u8(0xD3)
            _emit_add_sp_imm(b, 2)
            _emit_mov_ax_imm(b, 0)
            return

        arg_count = len(expr.args)
        i = arg_count - 1
        while i >= 0:
            _compile_expr(env, expr.args[i])
            _emit_push_ax(b)
            i -= 1

        if name in env.internal_funcs:
            b.emit_call_label(f"fn_{name}")
        else:
            idx = b.get_import_index(name, expr.line, expr.col)
            patch_pos = _emit_mov_bx_imm(b, 0)
            b.relocs.append(RelocEntry(patch_off=patch_pos, reloc_type=RELOC_IMPORT16, arg=idx))
            b.emit_u8(0xFF)
            b.emit_u8(0xD3)

        _emit_add_sp_imm(b, arg_count * 2)
        return

    raise CompileError(expr.line, expr.col, "unsupported expression")


def _elem_count(dims: tuple[int, ...]) -> int:
    total = 1
    for d in dims:
        total *= d
    return total


def _flatten_scalar_initializer(init: InitNode, line: int, col: int) -> Expr | None:
    if isinstance(init, InitExpr):
        return init.expr
    if isinstance(init, InitList):
        if len(init.items) == 0:
            return None
        if len(init.items) > 1:
            raise CompileError(line, col, "too many scalar initializer elements")
        return _flatten_scalar_initializer(init.items[0], line, col)
    return None


def _flat_index_to_indices(flat_idx: int, dims: tuple[int, ...]) -> list[int]:
    idxs: list[int] = [0] * len(dims)
    i = len(dims) - 1
    n = flat_idx
    while i >= 0:
        d = dims[i]
        idxs[i] = n % d
        n //= d
        i -= 1
    return idxs


def _build_array_elem_lvalue(name: str, dims: tuple[int, ...], flat_idx: int, line: int, col: int) -> Expr:
    idxs = _flat_index_to_indices(flat_idx, dims)
    expr: Expr = Var(line=line, col=col, name=name)
    for idx in idxs:
        expr = Index(
            line=line,
            col=col,
            base=expr,
            index=Number(line=line, col=col, value=idx),
        )
    return expr


def _build_array_elem_lvalue_from_base(base: Expr, dims: tuple[int, ...], flat_idx: int, line: int, col: int) -> Expr:
    idxs = _flat_index_to_indices(flat_idx, dims)
    expr: Expr = base
    for idx in idxs:
        expr = Index(
            line=line,
            col=col,
            base=expr,
            index=Number(line=line, col=col, value=idx),
        )
    return expr


def _emit_assign_to_lvalue(env: FunctionEnv, lhs: Expr, rhs: Expr | None, line: int, col: int) -> None:
    b = env.b
    lt = _lvalue_type(env, lhs)
    _compile_lvalue_address(env, lhs)
    _emit_push_ax(b)
    if rhs is None:
        _emit_mov_ax_imm(b, 0)
    else:
        _compile_expr(env, rhs)
    _emit_pop_bx(b)
    if _type_is_char_scalar(lt):
        _emit_store_u8_al_to_bx(b)
    else:
        b.emit_u8(0x89)
        b.emit_u8(0x07)


def _zero_init_for_type(env: FunctionEnv, lhs: Expr, ctype: CType, line: int, col: int) -> None:
    if len(ctype.array_dims) > 0:
        total = _elem_count(ctype.array_dims)
        elem_type = CType(
            base=ctype.base,
            ptr=ctype.ptr,
            struct_name=ctype.struct_name,
            array_dims=ctype.array_dims[1:],
        )
        i = 0
        while i < total:
            elem_lhs = _build_array_elem_lvalue_from_base(lhs, ctype.array_dims, i, line, col)
            _zero_init_for_type(env, elem_lhs, elem_type, line, col)
            i += 1
        return

    if _type_is_struct_value(ctype):
        info = env.struct_infos.get(ctype.struct_name or "")
        if info is None:
            raise CompileError(line, col, f"unknown struct '{ctype.struct_name}'")
        for field_name, field in info.fields.items():
            field_lhs = Member(line=line, col=col, base=lhs, field=field_name, via_ptr=False)
            _zero_init_for_type(env, field_lhs, field.ctype, line, col)
        return

    _emit_assign_to_lvalue(env, lhs, None, line, col)


def _emit_initializer_for_type(env: FunctionEnv, lhs: Expr, ctype: CType, init: InitNode, line: int, col: int) -> None:
    if len(ctype.array_dims) > 0:
        items = init.items if isinstance(init, InitList) else [init]
        n0 = ctype.array_dims[0]
        if len(items) > n0:
            raise CompileError(line, col, "too many array initializer elements")

        elem_type = CType(
            base=ctype.base,
            ptr=ctype.ptr,
            struct_name=ctype.struct_name,
            array_dims=ctype.array_dims[1:],
        )
        i = 0
        while i < n0:
            elem_lhs = Index(line=line, col=col, base=lhs, index=Number(line=line, col=col, value=i))
            if i < len(items):
                _emit_initializer_for_type(env, elem_lhs, elem_type, items[i], line, col)
            else:
                _zero_init_for_type(env, elem_lhs, elem_type, line, col)
            i += 1
        return

    if _type_is_struct_value(ctype):
        info = env.struct_infos.get(ctype.struct_name or "")
        if info is None:
            raise CompileError(line, col, f"unknown struct '{ctype.struct_name}'")

        if isinstance(init, InitExpr):
            src_t = _expr_type(env, init.expr)
            if (
                _type_is_struct_value(src_t)
                and src_t.base == ctype.base
                and src_t.ptr == ctype.ptr
                and src_t.struct_name == ctype.struct_name
                and src_t.array_dims == ctype.array_dims
            ):
                _compile_struct_copy(env, lhs, init.expr, _type_size(ctype, env.struct_infos, line, col))
                return
            raise CompileError(line, col, "invalid struct initializer expression")

        items = init.items if isinstance(init, InitList) else [init]
        fields = list(info.fields.items())
        if len(items) > len(fields):
            raise CompileError(line, col, "too many struct initializer elements")
        i = 0
        while i < len(fields):
            field_name, field = fields[i]
            field_lhs = Member(line=line, col=col, base=lhs, field=field_name, via_ptr=False)
            if i < len(items):
                _emit_initializer_for_type(env, field_lhs, field.ctype, items[i], line, col)
            else:
                _zero_init_for_type(env, field_lhs, field.ctype, line, col)
            i += 1
        return

    expr = _flatten_scalar_initializer(init, line, col)
    _emit_assign_to_lvalue(env, lhs, expr, line, col)


def _compile_decl_initializer(env: FunctionEnv, decl: VarDecl) -> None:
    slot = env.find_var(decl.name, decl.line, decl.col)
    lhs = Var(line=decl.line, col=decl.col, name=decl.name)
    if decl.init is None:
        if len(slot.array_dims) > 0 or _type_is_struct_value(slot.ctype):
            return
        _emit_assign_to_lvalue(env, lhs, None, decl.line, decl.col)
        return

    _emit_initializer_for_type(env, lhs, slot.ctype, decl.init, decl.line, decl.col)


def _compile_stmt(env: FunctionEnv, stmt) -> None:
    b = env.b

    if isinstance(stmt, BlockStmt):
        for child in stmt.statements:
            _compile_stmt(env, child)
        return

    if isinstance(stmt, VarDecl):
        _compile_decl_initializer(env, stmt)
        return

    if isinstance(stmt, ExprStmt):
        if stmt.expr is not None:
            _compile_expr(env, stmt.expr)
        return

    if isinstance(stmt, ReturnStmt):
        if stmt.expr is None:
            _emit_mov_ax_imm(b, 0)
        else:
            _compile_expr(env, stmt.expr)
        b.emit_jmp_label(env.epilogue_label)
        return

    if isinstance(stmt, IfStmt):
        else_label = b.new_label("if_else")
        end_label = b.new_label("if_end")

        _compile_expr(env, stmt.cond)
        _emit_jump_if_ax_zero(b, else_label)
        _compile_stmt(env, stmt.then_branch)

        if stmt.else_branch is not None:
            b.emit_jmp_label(end_label)
            b.mark_label(else_label)
            _compile_stmt(env, stmt.else_branch)
            b.mark_label(end_label)
        else:
            b.mark_label(else_label)

        return

    if isinstance(stmt, WhileStmt):
        start_label = b.new_label("while_start")
        end_label = b.new_label("while_end")

        b.mark_label(start_label)
        _compile_expr(env, stmt.cond)
        _emit_jump_if_ax_zero(b, end_label)

        env.break_labels.append(end_label)
        env.continue_labels.append(start_label)
        _compile_stmt(env, stmt.body)
        env.continue_labels.pop()
        env.break_labels.pop()

        b.emit_jmp_label(start_label)
        b.mark_label(end_label)
        return

    if isinstance(stmt, ForStmt):
        start_label = b.new_label("for_start")
        step_label = b.new_label("for_step")
        end_label = b.new_label("for_end")

        if stmt.init is not None:
            if isinstance(stmt.init, VarDecl):
                _compile_stmt(env, stmt.init)
            else:
                _compile_expr(env, stmt.init)

        b.mark_label(start_label)
        if stmt.cond is not None:
            _compile_expr(env, stmt.cond)
            _emit_jump_if_ax_zero(b, end_label)

        env.break_labels.append(end_label)
        env.continue_labels.append(step_label)
        _compile_stmt(env, stmt.body)
        env.continue_labels.pop()
        env.break_labels.pop()

        b.mark_label(step_label)
        if stmt.step is not None:
            _compile_expr(env, stmt.step)

        b.emit_jmp_label(start_label)
        b.mark_label(end_label)
        return

    if isinstance(stmt, BreakStmt):
        if not env.break_labels:
            raise CompileError(stmt.line, stmt.col, "break outside loop")
        b.emit_jmp_label(env.break_labels[-1])
        return

    if isinstance(stmt, ContinueStmt):
        if not env.continue_labels:
            raise CompileError(stmt.line, stmt.col, "continue outside loop")
        b.emit_jmp_label(env.continue_labels[-1])
        return

    raise CompileError(stmt.line, stmt.col, "unsupported statement")


def _compile_function(
    builder: CodeBuilder,
    fn: FunctionDef,
    internal_funcs: set[str],
    struct_infos: dict[str, StructInfo],
    global_vars: dict[str, VarSlot],
    global_init_decls: list[VarDecl],
) -> None:
    env = FunctionEnv(builder, fn, internal_funcs, struct_infos, global_vars)
    local_size = _prepare_var_slots(env)

    builder.mark_label(f"fn_{fn.name}")

    builder.emit_u8(0x55)
    builder.emit_u8(0x89)
    builder.emit_u8(0xE5)

    if local_size > 0:
        builder.emit_u8(0x81)
        builder.emit_u8(0xEC)
        builder.emit_u16(local_size)

    if fn.name == "main":
        for gdecl in global_init_decls:
            _compile_decl_initializer(env, gdecl)

    _compile_stmt(env, fn.body)

    _emit_mov_ax_imm(builder, 0)

    builder.mark_label(env.epilogue_label)
    builder.emit_u8(0x89)
    builder.emit_u8(0xEC)
    builder.emit_u8(0x5D)
    builder.emit_u8(0xC3)


def generate(program: Program, _symbols: dict[str, int]) -> CodegenResult:
    del _symbols

    b = CodeBuilder()
    struct_infos = _build_struct_layouts(program)
    internal_funcs = {fn.name for fn in program.functions}
    global_vars: dict[str, VarSlot] = {}
    global_bss_size = 0
    global_init_decls: list[VarDecl] = []

    for g in program.globals:
        ct = _type_from_spec(g.type_spec)
        ct = CType(base=ct.base, ptr=ct.ptr, struct_name=ct.struct_name, array_dims=g.array_dims)
        size = _type_size(ct, struct_infos, g.line, g.col)
        global_vars[g.name] = VarSlot(
            is_param=False,
            is_global=True,
            offset=global_bss_size,
            size=size,
            ctype=ct,
            array_dims=g.array_dims,
        )
        global_bss_size += size
        if g.init is not None:
            global_init_decls.append(g)

    for fn in program.functions:
        _compile_function(b, fn, internal_funcs, struct_infos, global_vars, global_init_decls)

    return b.finalize("fn_main", global_bss_size)
