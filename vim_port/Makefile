SHELL := /bin/sh

# Makefile for vim port to seolsem
# Uses Open Watcom C compiler

# Compiler settings
# -zq: Operate quietly (suppress banner)
# -zl: Suppress default library file names
ifeq ($(origin CC),default)
CC := wcc
endif
CFLAGS ?= -s -ecc -1 -ms -zl -zq -os -d0

# Include paths: our stubs first, then vim source, then vim proto
INCLUDES = -i. -i$(VIM_SRC) -i$(VIM_SRC)/proto

# Preprocessor defines
DEFINES = -DMSDOS -DDOS16 -DFEAT_TINY -DSEOLSEM -DSMALL_MALLOC \
          -DHAVE_STDLIB_H -DHAVE_STRING_H -DHAVE_STRCSPN -DHAVE_STRICMP \
          -DHAVE_STRNICMP -DHAVE_MEMSET -DHAVE_QSORT

# Vim source directory
# The historical source tree lived at ../vim/SOURCE/VIM/src.
# In current repository state, ../vim may be removed; in that case we build only
# the seolsem shim objects unless VIM_SRC is provided explicitly.
VIM_SRC ?= ../vim/SOURCE/VIM/src
VIM_AVAILABLE := $(if $(wildcard $(VIM_SRC)/version.c),1,0)

# Output directory
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
SHIM_STAMP = $(BUILD_DIR)/.shim_ready

# Object files - seolsem shim
SHIM_OBJS = \
	$(OBJ_DIR)/seolsem_libc.obj \
	$(OBJ_DIR)/os_seolsem.obj

# Core vim objects (minimal set for FEAT_TINY)
VIM_OBJS = \
	$(OBJ_DIR)/version.obj \
	$(OBJ_DIR)/charset.obj \
	$(OBJ_DIR)/misc1.obj \
	$(OBJ_DIR)/misc2.obj \
	$(OBJ_DIR)/screen.obj \
	$(OBJ_DIR)/message.obj \
	$(OBJ_DIR)/memfile.obj \
	$(OBJ_DIR)/memline.obj \
	$(OBJ_DIR)/getchar.obj \
	$(OBJ_DIR)/move.obj \
	$(OBJ_DIR)/mark.obj \
	$(OBJ_DIR)/normal.obj \
	$(OBJ_DIR)/ops.obj \
	$(OBJ_DIR)/option.obj \
	$(OBJ_DIR)/search.obj \
	$(OBJ_DIR)/regexp.obj \
	$(OBJ_DIR)/term.obj \
	$(OBJ_DIR)/ui.obj \
	$(OBJ_DIR)/undo.obj \
	$(OBJ_DIR)/edit.obj \
	$(OBJ_DIR)/ex_cmds.obj \
	$(OBJ_DIR)/ex_cmds2.obj \
	$(OBJ_DIR)/ex_docmd.obj \
	$(OBJ_DIR)/ex_getln.obj \
	$(OBJ_DIR)/fileio.obj \
	$(OBJ_DIR)/buffer.obj \
	$(OBJ_DIR)/tag.obj \
	$(OBJ_DIR)/window.obj \
	$(OBJ_DIR)/syntax.obj \
	$(OBJ_DIR)/main.obj

ifeq ($(VIM_AVAILABLE),1)
ALL_OBJS = $(SHIM_OBJS) $(VIM_OBJS)
else
ALL_OBJS = $(SHIM_OBJS)
endif

# Targets
.PHONY: all clean dirs test test2 showdefs vim-core shim help

all: dirs $(ALL_OBJS) $(SHIM_STAMP)
ifeq ($(VIM_AVAILABLE),1)
	@echo "Vim build complete"
else
	@echo "Shim build complete (vim source not found at $(VIM_SRC))"
	@echo "To build vim core objects, set VIM_SRC to a valid vim src path."
endif

shim: $(SHIM_STAMP)
	@echo "Shim build complete"

$(SHIM_STAMP): dirs $(SHIM_OBJS)
	@touch $@

ifeq ($(VIM_AVAILABLE),1)
vim-core: dirs $(VIM_OBJS)
	@echo "Vim core objects build complete"
else
vim-core:
	@echo "Skipped: vim source not found at $(VIM_SRC)"
	@echo "Set VIM_SRC=/path/to/vim/src and run: make vim-core"
endif

dirs:
	@mkdir -p $(OBJ_DIR)

clean:
	@rm -rf $(BUILD_DIR)
	@rm -f ./*.err

# Compilation rules for shim
$(OBJ_DIR)/seolsem_libc.obj: seolsem_libc.c seolsem_libc.h
	$(CC) $(CFLAGS) $(DEFINES) -i. seolsem_libc.c -fo=$@

$(OBJ_DIR)/os_seolsem.obj: os_seolsem.c os_seolsem.h seolsem_libc.h
	$(CC) $(CFLAGS) $(DEFINES) -i. os_seolsem.c -fo=$@

# Generic rule for vim source files
$(OBJ_DIR)/%.obj: $(VIM_SRC)/%.c
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) $< -fo=$@

# Test target - syntax check only
ifeq ($(VIM_AVAILABLE),1)
test:
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -zs $(VIM_SRC)/version.c

test2:
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -zs $(VIM_SRC)/charset.c
else
test test2:
	@echo "Skipped: vim source not found at $(VIM_SRC)"
endif

# Show defines for debugging
showdefs:
	@echo "CFLAGS: $(CFLAGS)"
	@echo "DEFINES: $(DEFINES)"
	@echo "INCLUDES: $(INCLUDES)"
	@echo "VIM_SRC: $(VIM_SRC)"
	@echo "VIM_AVAILABLE: $(VIM_AVAILABLE)"

help:
	@echo "Targets:"
	@echo "  all       - Build shim + vim objects if VIM_SRC is available."
	@echo "  shim      - Build only seolsem shim objects."
	@echo "  vim-core  - Build vim core objects (requires valid VIM_SRC)."
	@echo "  clean     - Remove build outputs and local *.err files."
