.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT6797 CCCI/CLDMA WWAN architecture
=============================================

Scope
-----

The MT6797 application processor talks to MD1 through MediaTek's CCCI packet
protocol carried by the on-SoC CLDMA engine.  CCCI is a firmware ABI, not a
userspace ABI.  A mainline driver should expose the standard WWAN AT port and
raw-IP WWAN links and keep CCCI framing, queue selection and lifecycle control
inside the kernel.

The modem does not speak MBIM or QMI on this transport.  Labelling CCCI bytes
as either protocol would make userspace probe an incompatible wire format.
The control surface is therefore ``WWAN_PORT_AT``.  SIM PIN, network
registration, PDP context and eSIM APDU/provisioning operations belong in an
AT-capable userspace manager such as ModemManager; no Android CCCI ioctl ABI is
part of this design.

Source-derived wire contract
----------------------------

The following facts come from the Gemini 3.18 vendor source.  Paths are
relative to ``drivers/misc/mediatek/eccci`` unless otherwise stated.

* ``../include/mt-plat/mt_ccci_common.h:78-88`` defines the packed, 16-byte
  CCCI header: two data words, a 16-bit channel, 15-bit sequence number,
  assertion bit and reserved word.  ``ccci_modem.h:160-173`` assigns transmit
  sequence numbers under the hardware-queue lock.
* ``modem_cldma.h:258-285`` defines 16-byte TGPD and RGPD descriptors.
  Pointer bits 31:0 are in the pointer word and bits 35:32 are in the low or
  high nibble of the descriptor debug field.  ``modem_cldma.c:909-968`` forms
  cyclic rings.  HWO is bit 0 and IOC is bit 7.
* ``mt6797/cldma_reg.h:108-186`` places uplink queue start/current and command
  registers in the power-down bank.  ``mt6797/cldma_reg.h:47-105`` places
  downlink queue start/current and RX interrupt masks in the always-on bank.
  ``mt6797/cldma_reg.h:220-257`` defines the shared interrupt status and mask
  registers.  Its queue-start helpers at lines 276-289 write only the low
  32 address bits.  Coherent GPD roots therefore stay below 4 GiB, while
  streaming payload mappings may use the descriptor's 36-bit packed pointer.
* ``modem_cldma.c:1494-1530`` programs all ring heads, issues a write barrier,
  starts queues 0 through 6 with mask ``0x7f`` and unmasks DONE and ERROR
  interrupts.  ``modem_cldma.c:1115-1221`` acknowledges status before handing
  completed queues to workers and masks RX DONE until draining completes.
* ``port_cfg.c:19-88`` maps the control channel to queue 0, the primary modem
  AT/MUX channel (UART2) to queue 1, modem log to queue 2, data links 0, 1 and
  7 to queue 3, and the remaining data links to queue 5.  It defines 18 CCMNI
  raw-IP contexts, with index 8 absent from this firmware's channel map.
* ``port_net.c:246-275`` prepends a CCCI header whose first data word is zero,
  second data word is the complete framed length and channel is the selected
  CCMNI TX channel.  ``port_net.c:376-423`` removes that header before
  delivering raw IP.
* ``port_ctlmsg.c:26-40`` and ``port_proxy.c:1636-1675`` define HS1/HS2.
  A CONTROL_RX frame with message zero and reserved value ``0x5555ffff`` is
  HS1; a 16-byte frame requests the legacy runtime block and an 88-byte frame
  requests feature-negotiated v2 runtime data.  A subsequent control message
  zero is HS2 and makes data/control ports ready.
* ``port_proxy.c:1636-1659`` calls
  ``ccci_set_mem_access_protection_second_stage()`` after decoding HS1 and
  before sending AP runtime data.  ``mt6797/ccci_platform.c:579-605`` shows
  that this rewrites only CHECK_HEADER region 2 / EMI region 13 with its final
  permission.  It cannot be skipped merely because LK programmed that tuple
  before stage one temporarily replaced it.
* ``ccci_core.h:309-402`` defines the v2 feature-query and runtime-feature
  records.  ``ccci_modem.c:592-851`` validates the query patterns, negotiates
  each feature, and writes AP runtime records into shared memory.
* ``arch/arm64/boot/dts/mt6797.dtsi:1782-1817`` supplies six register windows,
  CLDMA/CCIF/WDT interrupts 265/147/266, capability 6 and a 1 MiB MD1 shared
  memory requirement.
* ``../ccci_util/ccci_util_lib_load_img.c:729-873`` implements the Linux
  ``request_firmware()`` fallback for MD, DSP and ARM7 split images.  Lines
  923-946 and 1005-1019 define the optional 512-byte partition wrapper; lines
  424-449 bind the trailing CHECK_HEADER ``md_img_size`` to the loaded image.
  ``modem_cldma.c:2064-2108`` places auxiliaries only at header-supplied offsets
  before MPU, power and boot-vector release.

Driver boundaries
-----------------

The implementation is divided into four ownership layers:

1. The MD1 lifecycle owner validates the LK-loaded image and CCCI carveouts.
   Its independently gated ``firmware-name`` path can instead request and
   validate a complete MD image plus every header-required DSP/ARM7 image.  It
   rechecks the reset-held proof after firmware acquisition, clears the MD
   reservation, copies only wrapper-declared bytes, then establishes MPU/remap,
   powers MD1 and releases its boot vector.  Transport is allowed to attach
   only after completed post-vector handoff and an exclusive, reset-completed
   AP register lease. The supplier retains the register reservations and
   drains its observer before handing over access. Fresh queue, IRQ, busy and
   head checks must pass before the transport's first register write.
2. The CLDMA transport owns the AP-side register windows, IRQ and coherent GPD
   rings.  It uses DMA API mappings, per-TX-ring locks and NAPI for RX/TX
   completion.  Descriptor contents are published before HWO with ``dma_wmb``;
   completion reads HWO before consuming descriptor fields with ``dma_rmb``.
3. The CCCI router validates lengths and channels, performs sequence tracking,
   completes HS1/HS2, and routes payloads.  HS1 runs in sleepable context so
   the final one-way region-13 secure SET completes before the runtime reply.
   Unknown channels are counted and dropped; they are never surfaced through a
   generic private character device.
4. The WWAN frontend exposes UART2 as ``WWAN_PORT_AT`` and CCMNI sessions as
   point-to-point ``ARPHRD_NONE`` links.  Link identifiers map directly to
   firmware PDP contexts, so standard WWAN rtnetlink creates and removes
   additional contexts.

Failure model
-------------

Malformed CCCI or GPD lengths are isolated to the packet and counted.  A
duplicate or invalid HS1, a 30-second HS2 timeout, CLDMA error, region-13 SET
failure or MD WDT event masks every queue, stops netdev carrier and prevents new
TX.  The lifecycle owner latches every post-release failure: the source does
not prove that an in-boot reset can safely undo power, remap and one-way secure
MPU state.  Recovery therefore requires a platform reset, followed by firmware
validation, descriptor reinitialization and a fresh HS1/HS2 exchange.

Known hardware-validation gaps
-------------------------------

Registration and packet service require a usable SIM/subscription and must
be validated separately from transport bring-up; their status is not inferred
from historical campaign notes. Hardware validation must still prove
CLDMA interrupt polarity, all queue head/current transitions, HS1 v2 contents,
AT command/response flow, raw IPv4/IPv6 delivery, WDT quiesce and a full
lifecycle-owner recovery.  The vendor SIM switch at
``drivers/misc/mediatek/ssw/ssw_single_v2/sim_switch.c:168-193`` proves only
physical pinctrl routing to MD1.  No EID, eUICC profile, LPA or eSIM operation
was found in the vendor or Gemini userspace sources.  eSIM support therefore
cannot be claimed until ``WWAN_PORT_AT`` answers the established 3GPP logical
channel/APDU commands and an LPA can read an EID.  A failed capability probe
must leave ordinary physical-SIM ModemManager operation unchanged.
