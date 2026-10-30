# Intel iwx hardware reference

Bootstrap context layouts in iwx_boot_regs.h are the prefix of OpenBSD
sys/dev/pci/if_iwxreg.h, before IWX_UCODE_TLV_INST, from commit
65596250e06438442ce07bad87e5ba129229607d. Original notices are retained;
this copy uses the BSD licence offered by the source's dual license.

Source:
https://github.com/openbsd/src/blob/65596250e06438442ce07bad87e5ba129229607d/sys/dev/pci/if_iwxreg.h

Native firmware parsing, DMA contexts and hardware bootstrap helpers are in
net/iwx_fw.c, net/iwx_boot.c and net/iwx_hw.c. They are not wired to PCI
discovery and do not advertise Wi-Fi connectivity. ALIVE/PNVM notification handling, NVM,
command/RX/TX transport and association remain pending. Original source
copyright/licence notices are also reproduced in NOTICES.
