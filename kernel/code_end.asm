bits 16

; Marker placed at the end of the CODE segment.
segment _TEXT class=CODE use16
global _code_end_marker
_code_end_marker:
    db "CODE_END_v1", 0
