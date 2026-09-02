.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT6797 SENINF receiver
===============================

Evidence boundary
-----------------

The MT6797 vendor device tree describes SENINF0 through SENINF7 as consecutive
4 KiB windows at ``0x1a040000`` through ``0x1a047000`` and the two MIPI analog
windows at ``0x10217000`` and ``0x10218000``. The vendor ISP driver maps only
SENINF0 through SENINF3 and acquires the ``ISP_CAM_SENINF`` clock as part of a
larger private ISP transaction. No independent SENINF interrupt is described.

The available MediaTek ISP 5.0 reference driver contains a complete MT8183
D-PHY sequence. It is not evidence that those analog values, port mapping, or
settle timings apply to MT6797. The MT6797 vendor kernel only preserves and
restores receiver registers; the initial programming contract could not be
determined.

Current capability
------------------

The driver therefore provides only the side-effect-free milestone:

* validate and reserve the complete receiver and analog resource inventory;
* validate an explicit one-to-one CSI-2 D-PHY media endpoint;
* expose a V4L2 bridge with the source-proven SP5509 10-bit ``GRBG`` format;
* never enable the SENINF clock and never read or write receiver registers;
* reject stream-on with ``-EOPNOTSUPP``.

The MT6797 SoC node is disabled. Adding a sensor endpoint alone is not enough
to make capture safe: the physical receiver route, D-PHY power sequence,
settle timing, capture-engine IRQ, and IOMMU/DMA ownership remain separate
acceptance gates.
