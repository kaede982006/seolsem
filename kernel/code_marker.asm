bits 16

; Marker placed at the very start of the CODE segment.
segment _TEXT class=CODE use16
global _code_marker
_code_marker:
    db "CODE_MARKER_v1", 0
