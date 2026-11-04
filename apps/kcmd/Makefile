# SPDX-License-Identifier: GPL-3.0-or-later
APP         = kcmd
TITLE       = Banana OS commands
VERSION     = 1.0
TYPE        = console
DESCRIPTION = The system's own commands as programs in /bin: startx, install, pkg, ifconfig, theme, selftest...
AUTHOR      = Banana OS
CATEGORY    = System
SRCS        = main.c

# every command of the system's help (shell/help.c), less those bash, GNU
# coreutils and the other ports give (theirs are the ones in /bin)
NOT_THESE   = ls cd pwd mkdir rm touch cp mv find chmod chown chgrp ln readlink stat cat grep wc head tail \
              echo uname whoami date sleep nice time clear history type alias unalias export unset \
              env exit run which kill nano vim vi nvim neofetch
COMMANDS   := $(filter-out $(NOT_THESE),$(shell awk '/^static const page_t PAGES\[\] = \{/{f=1;next} f&&/^};/{f=0} f' \
              ../../shell/help.c | sed -n 's/^    { "\([^"]*\)", "[a-z]*".*/\1/p'))
comma      := ,
empty      :=
space      := $(empty) $(empty)
BIN         = kcmd,$(subst $(space),$(comma),$(strip $(COMMANDS)))

include ../../sdk/banana.mk

# The command aliases are generated from the system help catalog.
$(BUILD)/manifest: ../../shell/help.c
