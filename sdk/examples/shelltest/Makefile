# SPDX-License-Identifier: BSD-2-Clause
APP         = shelltest
TITLE       = Shell test
VERSION     = 1.0
TYPE        = console
DESCRIPTION = Checks GNU bash and zsh: scripts against what they print on Linux, and typed at on a terminal
AUTHOR      = Banana OS SDK
CATEGORY    = Extras
SRCS        = shelltest.c interactive.c
DATA        = run.sh $(wildcard tests/*.sh tests/*.zsh tests/*.out)

include ../../banana.mk
