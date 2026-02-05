bits 16

; DGROUP marker + its own offset (in DGROUP) to recover DGROUP base even if BSS precedes DATA.
segment _DATA class=DATA use16
global _dgroup_marker
_dgroup_marker:
    db "DGROUP_MARKER_v2", 0
    dw _dgroup_marker
