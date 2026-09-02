.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT6797 SENINF receiver
===============================

Recovered contract
------------------

The MT6797 vendor device tree describes SENINF0 through SENINF7 as consecutive
4 KiB windows at ``0x1a040000`` through ``0x1a047000`` and the two MIPI analog
windows at ``0x10217000`` and ``0x10218000``. The vendor ISP driver maps only
SENINF0 through SENINF3 and acquires the ``ISP_CAM_SENINF`` clock as part of a
larger private ISP transaction. No independent SENINF interrupt is described.

The programming contract was recovered from both the MT6797 vendor source and
the immutable 64-bit Gemini stock camera libraries. ``libcam.halsensor.so``
(SHA-256 ``15e8e8aaf2ece4ad1cb73e59633be254830c9433759cba88c0b6a036b7f32613``)
provides the MT6797-specific D-PHY setup, calibration and mux operations.
``libcameracustom.so``
(SHA-256 ``9974ca399e2733f9ad2d98d72c7d2cff99fd67c3126534a45e39b0c0ecd2cc44``)
maps the sub-camera device ID to MIPI port 1.

For the populated Gemini camera1/sub route this establishes:

* SENINF instance 1, NCSI2 source 8 and mux 1 routed to top output 0;
* CSI1A and CSI1B analog lanes in the CSI0 analog resource;
* physical data lanes 1 and 2, RAW10 GRBG, low clock and sync polarities;
* manual settle delay 14, converted by the HAL to register count 4;
* devinfo words ``0x25`` through ``0x29``, with port 1 calibration packed
  from words ``0x27`` and ``0x28``;
* analog power delays of 30 us then 1 us, followed by a 500 us NCSI2
  calibration interval;
* calibration completion masks ``0x10001`` and ``0x101``.

The vendor SP5509 source distinguishes a 640x480 high-speed mode with settle
delay 19. Only the 2592x1944 mode at link frequency 440 MHz is enabled here;
the 1296x972 and 640x480 register plans remain outside this milestone.

Current capability
------------------

The driver provides the smallest complete, fail-closed receiver milestone:

* validate and reserve the complete receiver and analog resource inventory;
* require the exact two-lane ``<1 2>`` Gemini route;
* expose a V4L2 bridge with the source-proven SP5509 10-bit ``GRBG`` format;
* consume the five camera calibration words from the read-only LK devinfo
  NVMEM provider;
* power, calibrate and route NCSI2 only while the receiver is streaming;
* reject an unproved mode or link frequency with ``-EOPNOTSUPP``;
* roll the receiver, mux and analog power bits back if calibration fails.

The companion CAMSV0 driver completes the standard media graph from the
SP5509 sensor through SENINF to a DMA-contiguous V4L2 capture queue.  It owns
the recovered CAMSV0 interrupt, clocks, reset and packed RAW10 register plan;
unsupported formats and dimensions are rejected.

The complete Gemini route remains disabled in Device Tree until it has passed
an on-device stream and frame-integrity test.  Enable ``i2c3``, ``sp5509``,
``larb2``, ``seninf`` and ``camsv0`` together; a partial route is neither safe
nor useful.
