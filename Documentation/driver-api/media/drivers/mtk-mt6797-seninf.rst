.. SPDX-License-Identifier: GPL-2.0-only

MediaTek MT6797 camera recovery
===============================

Evidence boundary
-----------------

The recovered implementation is based on the MT6797 vendor kernel, the
immutable 64-bit Gemini stock camera libraries and a recorded stock runtime
identity. No Gemini camera schematic was available and no mainline capture
has been performed.

The stock runtime reports ``AEON_CAMERA0=non_sensor`` and
``AEON_CAMERA1=sp5509mipirawsls``. The vendor sensor-list code assigns
``AEON_CAMERA1`` only after a successful logical sub-camera probe. This is
direct evidence that the populated logical sub camera selects the SiliconFile
SP5509 SLS driver. Other sensor names compiled into the stock libraries are
alternatives, not evidence for additional populated sensors.

The logical sub-camera mapping in the vendor source gives these board design
facts:

* I2C controller 3 at 300 kHz;
* MCLK2 at 24 MHz on GPIO31;
* active-low reset on GPIO33 and active-low power-down on GPIO29;
* PMIC DOVDD 1.8 V, AVDD 2.8 V and DVDD 1.2 V supplies; and
* a two-data-lane NCSI2 connection through SENINF instance 1.

It does not establish the populated sensor's I2C address. The SLS probe table
tries vendor 8-bit write IDs ``0x40`` and ``0x50``, which correspond to Linux
7-bit addresses ``0x20`` and ``0x28``. The surviving runtime record does not
say which address responded. Consequently the Gemini base Device Tree does
not describe a sensor client, camera pinctrl state or media links. A disabled
node with a guessed address would still assert incorrect hardware topology and
would be copied into future activation overlays.

Recovered SoC contracts
-----------------------

The vendor Device Tree describes CAMSYS at ``0x1a000000``, camera LARB2 at
``0x1a001000``, SENINF0 through SENINF7 as consecutive 4 KiB windows starting
at ``0x1a040000``, and MIPI analog windows at ``0x10217000`` and
``0x10218000``. It describes CAMSV00 through CAMSV21 at ``0x1a050000``
through ``0x1a055000``. Only CAMSV0 is represented by the current mainline
milestone because the vendor clock and M4U contracts establish three CAMSV
channels, not all six address windows.

The SENINF programming sequence was correlated with
``libcam.halsensor.so`` (SHA-256
``15e8e8aaf2ece4ad1cb73e59633be254830c9433759cba88c0b6a036b7f32613``)
and ``libcameracustom.so`` (SHA-256
``9974ca399e2733f9ad2d98d72c7d2cff99fd67c3126534a45e39b0c0ecd2cc44``).
For the recovered sub-camera route it programs SENINF1, NCSI2 source 8 and
mux 1 to top output 0, using physical data lanes 1 and 2. It consumes devinfo
words ``0x25`` through ``0x29``; the port 1 analog calibration comes from
words ``0x27`` and ``0x28``. The supported plan uses RAW10 GRBG, a manual
settle delay of 14 and a 440 MHz link frequency at 2592x1944.

The CAMSV0 node owns its clock/reset set, interrupt, LARB2 relationship and
``M4U_PORT_CAM_SV0`` (LARB2 port 6). The remaining recovered LARB2 camera
ports are IMGO, RRZO, AAO, AFO, LSCI0, LSCI1, SV1, SV2, LCSO, UFEO, BPCI,
PDO and RAWI. LARB6 carries the image-processing DMA ports, including IMGI,
IMG2O, IMG3O, VIPI and DPE. These port inventories are contracts for future
drivers, not devices implemented by this milestone.

Software capability layers
--------------------------

Raw sensor capture
~~~~~~~~~~~~~~~~~~

The SP5509 sensor variant, SENINF receiver and CAMSV0 capture driver form a
standard V4L2 media-controller design. They implement one source-recovered
2592x1944 packed RAW10 GRBG plan. CAMSV uses a DMA-contiguous videobuf2 queue
and advertises MMAP and DMABUF I/O. Endpoint, format, resource, register-plan
and rollback helpers have KUnit coverage. This is a code-complete dormant raw
path; it is not hardware-validated capture support.

Image signal processing
~~~~~~~~~~~~~~~~~~~~~~~

There is no upstream-style MT6797 ISP driver in this tree. The vendor stack
combines CAM, DIP and image-processing blocks behind private ioctls and broad
MMIO mappings. That ABI is not suitable for transplantation. Bayer
demosaicing, colour processing, statistics and auto-control integration are
therefore out of scope for the raw CAMSV milestone.

Zero-copy display and encoding
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

DMABUF queue support is necessary but does not by itself prove a zero-copy
pipeline. Buffer formats, modifiers, cache coherency, fencing and importer
compatibility with DRM or the MT6797 encoder remain untested. No direct
camera-to-display or camera-to-encoder path is claimed.

Userspace integration
~~~~~~~~~~~~~~~~~~~~~

An enabled and validated graph would expose standard media and V4L2 nodes.
There is no libcamera pipeline handler, tuning data or camera application
integration for MT6797 in this tree. Those are separate milestones after raw
frame capture and integrity have been demonstrated.

Activation gate
---------------

The SoC SENINF and CAMSV inventory remains disabled. Before adding a Gemini
sensor node or activation overlay, establish the populated 7-bit I2C address
from a schematic, module documentation or an authorised non-destructive probe.
Then validate chip ID ``0x0556``, power sequencing, CSI calibration, streaming
and frame integrity. Only after that evidence exists should I2C3, the sensor,
LARB2, SENINF and CAMSV0 be enabled as one complete graph.
