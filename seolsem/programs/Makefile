SHELL := /bin/sh
PY ?= python3
SEOLC_TOOL ?= ../tools/seolc_host.py

SRC_DIR := src
BIN_DIR := bin
XC_SRCS := $(wildcard $(SRC_DIR)/*.XC)
XEF_BINS := $(patsubst $(SRC_DIR)/%.XC,$(BIN_DIR)/%.XEF,$(XC_SRCS))
INC_DIRS ?= include ../include
SEOLC_I_FLAGS := $(foreach d,$(INC_DIRS),-I $(d))
HDRS := $(foreach d,$(INC_DIRS),$(wildcard $(d)/*.XH))

.PHONY: all clean list rebuild

all: $(XEF_BINS)

$(BIN_DIR)/%.XEF: $(SRC_DIR)/%.XC $(SEOLC_TOOL) $(HDRS)
	@mkdir -p $(BIN_DIR)
	$(PY) $(SEOLC_TOOL) "$<" -o "$@" --target native16 $(SEOLC_I_FLAGS)

rebuild: clean all

clean:
	@echo "[CLEAN] programs"
	@rm -rf $(BIN_DIR)
list:
	@printf '%s\n' $(XEF_BINS)
