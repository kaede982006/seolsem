from dataclasses import dataclass

from .errors import CompileError


_KEYWORDS = {
    "int",
    "char",
    "void",
    "return",
    "if",
    "else",
    "while",
    "for",
    "break",
    "continue",
    "struct",
}
_MULTI = {
    "<<",
    ">>",
    "<=",
    ">=",
    "==",
    "!=",
    "&&",
    "||",
    "->",
}
_SINGLE = set("{}();,+-*/%&|^=<>![],:.")


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int
    col: int


class Lexer:
    def __init__(self, source: str):
        self.src = source
        self.n = len(source)
        self.i = 0
        self.line = 1
        self.col = 1

    def _peek(self, offset: int = 0) -> str:
        idx = self.i + offset
        if idx >= self.n:
            return ""
        return self.src[idx]

    def _adv(self) -> str:
        if self.i >= self.n:
            return ""
        ch = self.src[self.i]
        self.i += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _skip_ws_and_comments(self) -> None:
        while True:
            ch = self._peek()
            if ch in (" ", "\t", "\r", "\n"):
                self._adv()
                continue

            if ch == "/" and self._peek(1) == "/":
                self._adv()
                self._adv()
                while self._peek() not in ("", "\n"):
                    self._adv()
                continue

            if ch == "/" and self._peek(1) == "*":
                start_line = self.line
                start_col = self.col
                self._adv()
                self._adv()
                while True:
                    cur = self._peek()
                    if cur == "":
                        raise CompileError(start_line, start_col, "unterminated block comment")
                    if cur == "*" and self._peek(1) == "/":
                        self._adv()
                        self._adv()
                        break
                    self._adv()
                continue

            break

    def _lex_ident(self) -> Token:
        line, col = self.line, self.col
        out = []
        while True:
            ch = self._peek()
            if ch == "" or not (ch.isalnum() or ch == "_"):
                break
            out.append(self._adv())
        text = "".join(out)
        if text in _KEYWORDS:
            return Token("KEYWORD", text, line, col)
        return Token("IDENT", text, line, col)

    def _lex_number(self) -> Token:
        line, col = self.line, self.col
        out = []
        if self._peek() == "0" and self._peek(1) in ("x", "X"):
            out.append(self._adv())
            out.append(self._adv())
            while True:
                ch = self._peek()
                if ch == "" or not (ch.isdigit() or ("a" <= ch.lower() <= "f")):
                    break
                out.append(self._adv())
        else:
            while True:
                ch = self._peek()
                if ch == "" or not ch.isdigit():
                    break
                out.append(self._adv())
        return Token("NUMBER", "".join(out), line, col)

    def _lex_string(self) -> Token:
        line, col = self.line, self.col
        self._adv()  # opening quote
        out = []
        while True:
            ch = self._peek()
            if ch == "":
                raise CompileError(line, col, "unterminated string")
            if ch == '"':
                self._adv()
                break
            if ch == "\\":
                self._adv()
                esc = self._peek()
                if esc == "":
                    raise CompileError(line, col, "unterminated string escape")
                self._adv()
                if esc == "n":
                    out.append("\n")
                elif esc == "t":
                    out.append("\t")
                elif esc == "r":
                    out.append("\r")
                elif esc == '"':
                    out.append('"')
                elif esc == "\\":
                    out.append("\\")
                else:
                    out.append(esc)
                continue
            out.append(self._adv())
        return Token("STRING", "".join(out), line, col)

    def lex(self) -> list[Token]:
        tokens: list[Token] = []
        while True:
            self._skip_ws_and_comments()
            ch = self._peek()
            if ch == "":
                tokens.append(Token("EOF", "", self.line, self.col))
                return tokens
            if ch.isalpha() or ch == "_":
                tokens.append(self._lex_ident())
                continue
            if ch.isdigit():
                tokens.append(self._lex_number())
                continue
            if ch == '"':
                tokens.append(self._lex_string())
                continue

            line, col = self.line, self.col
            two = ch + self._peek(1)
            if two in _MULTI:
                self._adv()
                self._adv()
                tokens.append(Token("SYMBOL", two, line, col))
                continue
            if ch in _SINGLE:
                self._adv()
                tokens.append(Token("SYMBOL", ch, line, col))
                continue
            raise CompileError(line, col, f"unexpected character: {ch!r}")
