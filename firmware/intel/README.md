# Intel firmware inputs for the native Wi-Fi port

Unmodified firmware binaries are pinned by full source revision, path, size
and SHA-256 in manifest.json. Redistribution conditions and Intel's disclaimer
are retained verbatim in LICENCE.iwlwifi_firmware.

AX200 uses iwlwifi-cc-a0-77.ucode; AX210 uses
iwlwifi-ty-a0-gf-a0-77.ucode and iwlwifi-ty-a0-gf-a0.pnvm.

Source: https://kernel.googlesource.com/pub/scm/linux/kernel/git/iwlwifi/linux-firmware/

These files are regression inputs for firmware parsing and DMA preparation.
Exact PNVM identity selection is also tested. Radio initialization and connectivity remain pending. The binaries are not
embedded in the ISO at this stage. No firmware is disassembled or modified.
