class CompileError(Exception):
    def __init__(self, line: int, col: int, message: str):
        super().__init__(message)
        self.line = line
        self.col = col
        self.message = message

    def format(self, path: str) -> str:
        return f"{path}:{self.line}:{self.col}: {self.message}"
