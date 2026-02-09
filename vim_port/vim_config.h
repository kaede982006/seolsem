/*
 * vim_config.h - Feature configuration for vim on seolsem
 * Overrides feature.h settings for minimal FEAT_TINY build
 */

#ifndef VIM_CONFIG_H
#define VIM_CONFIG_H

/* Use seolsem platform */
#define SEOLSEM         1
#define MSDOS           1
#define DOS16           1

/* 16-bit integers */
#define SIZEOF_INT      2
#define SMALL_MALLOC    1

/* Feature level - use TINY */
#define FEAT_TINY       1

/* Disable features that require more resources */
#undef FEAT_GUI
#undef FEAT_MOUSE
#undef FEAT_CLIPBOARD
#undef FEAT_MBYTE
#undef FEAT_EVAL
#undef FEAT_WINDOWS
#undef FEAT_QUICKFIX
#undef FEAT_VIMINFO
#undef FEAT_VISUAL
#undef FEAT_VERTSPLIT
#undef FEAT_FOLDING
#undef FEAT_DIFF
#undef FEAT_AUTOCMD
#undef FEAT_USR_CMDS
#undef FEAT_CMDL_COMPL
#undef FEAT_INS_EXPAND
#undef FEAT_LISTCMDS
#undef FEAT_SEARCHPATH
#undef FEAT_LINEBREAK
#undef FEAT_CINDENT
#undef FEAT_SMARTINDENT
#undef FEAT_COMMENTS
#undef FEAT_TITLE
#undef FEAT_TERMRESPONSE
#undef FEAT_SYN_HL
#undef FEAT_SPELL
#undef FEAT_CONCEAL
#undef FEAT_CRYPT
#undef FEAT_SIGNS
#undef FEAT_NETBEANS
#undef FEAT_CLIENTSERVER
#undef FEAT_PROFILE
#undef FEAT_RELTIME
#undef FEAT_PYTHON
#undef FEAT_PYTHON3
#undef FEAT_RUBY
#undef FEAT_PERL
#undef FEAT_TCL
#undef FEAT_LUA
#undef FEAT_MZSCHEME

/* Disable multi-byte and Unicode */
#undef FEAT_ARABIC
#undef FEAT_FARSI
#undef FEAT_FKMAP
#undef FEAT_RIGHTLEFT

/* Disable complex features */
#undef FEAT_PRINTER
#undef FEAT_POSTSCRIPT
#undef FEAT_XIM
#undef FEAT_KEYMAP
#undef FEAT_LANGMAP
#undef FEAT_DIGRAPH

/* Buffer and path limits */
#define IOSIZE          256
#define MAXPATHL        64
#define NUMBUFLEN       16
#define DIALOG_BUFFER   256

/* Don't use termcap database */
#define HAVE_TGETENT    0
#define TERMINFO        0

/* Simple terminal handling */
#define USE_TERM_CONSOLE 1

/* No swap files for simplicity */
#define FEAT_NOSWAPFILE 1

/* Enable basic features we need */
#define FEAT_CMDL_INFO  1
#define FEAT_EX_EXTRA   1
#define FEAT_MODIFY_FNAME 1

#endif /* VIM_CONFIG_H */
