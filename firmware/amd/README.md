# AMD Radeon microcode (Caicos: HD 6450 / 7450 / 8450; Turks: HD 6570 / 6670 / 7570 / 7670; Barts: HD 6790 / 6850 / 6870; Cedar: HD 5450 / 6350 / 7350)

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
| TURKS_pfp.bin  | Turks' prefetch parser            | 1120 |
| BARTS_pfp.bin  | Barts' prefetch parser            | 1120 |
| CEDAR_pfp.bin  | Cedar's prefetch parser           | 1120 |
| CEDAR_me.bin   | Cedar's micro engine              | 1376 |
| CEDAR_rlc.bin  | Cedar's run-list controller       | 768 |

Turks and Barts use CAICOS_me.bin and CAICOS_mc.bin: linux-firmware's
TURKS_me.bin, TURKS_mc.bin, BARTS_me.bin and BARTS_mc.bin are the same bytes
(SHA-256 37fa5fb7cdb13df94c9a64d7ba752b63119f063f64326b67ec54656cd184363b
for the ME, f88017d12951e68b8db2695350108f43b2bbac46fd4de525a1e2612540156a3d
for the MC), so they are not carried twice.
