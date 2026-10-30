# Intel iwx hardware reference

Bootstrap context layouts in iwx_boot_regs.h are the prefix of OpenBSD
sys/dev/pci/if_iwxreg.h, before IWX_UCODE_TLV_INST, from commit
65596250e06438442ce07bad87e5ba129229607d. Original notices are retained;
this copy uses the BSD licence offered by the source's dual license.

Source:
https://github.com/openbsd/src/blob/65596250e06438442ce07bad87e5ba129229607d/sys/dev/pci/if_iwxreg.h

Native firmware parsing, DMA contexts and hardware bootstrap helpers are in
net/iwx_fw.c, net/iwx_boot.c and net/iwx_hw.c. They are not wired to PCI
discovery and do not advertise Wi-Fi connectivity. Command/RX queue helpers
and bounded ALIVE decoding are in net/iwx_queue.c and net/iwx_msg.c.
The AX210 PNVM handshake is implemented in these helpers; PCI integration,
runtime regulatory/station data TX/RX and association remain pending.
NVM initialization helpers are in net/iwx_init.c and net/iwx_nvm.c. Original source
copyright/licence notices are also reproduced in NOTICES.


iwx_msg_regs.h selects the command descriptor, command header, RX packet and
ALIVE response layouts from the same pinned if_iwxreg.h revision above.
Original notices are retained under the BSD licence option.


iwx_nvm_regs.h retains NVM_GET_INFO v3/v4 response layouts and original
notices from the same pinned if_iwxreg.h revision. Native decoders use byte
reads and assert sizes/offsets against these packed definitions.
