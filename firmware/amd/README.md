# AMD Radeon microcode (Caicos: HD 6450 / 7450 / 8450; Cedar: HD 5450 / 6350 / 7350)

From linux-firmware (`radeon/`), unmodified, under AMD's redistribution
licence (`LICENSE.radeon`: binary form only, notice reproduced, no reverse
engineering). Loaded by `kernel/radeon_gfx.c` into the card's command
processor (PFP, ME), run-list controller (RLC) and memory controller (MC,
only when the video BIOS has not started it already).

| file | what | dwords |
|------|------|--------|
| CAICOS_pfp.bin | command processor prefetch parser | 1120 |
| CAICOS_me.bin  | command processor micro engine    | 1376 |
| BTC_rlc.bin    | run-list controller (Barts, Turks, Caicos) | 768 |
| CAICOS_mc.bin  | memory controller sequencer       | 6024 |
| CEDAR_pfp.bin  | Cedar's prefetch parser           | 1120 |
| CEDAR_me.bin   | Cedar's micro engine              | 1376 |
| CEDAR_rlc.bin  | Cedar's run-list controller       | 768 |
