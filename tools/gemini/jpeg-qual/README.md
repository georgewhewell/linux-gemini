# Gemini JPEG V4L2 qualification

`gemini-jpeg-qual` submits a deterministic 64x64 YUYV frame to a V4L2
memory-to-memory JPEG encoder, verifies the returned JPEG markers and
`bytesused`, then submits that JPEG to a V4L2 memory-to-memory decoder.  It
uses only standard V4L2 streaming, event, control, and MMAP ioctls.

Build and run the host-only checks:

    make -C tools/gemini/jpeg-qual check

For a diagnostic initramfs, build with its C compiler and copy the resulting
single binary:

    make -C tools/gemini/jpeg-qual CC=aarch64-linux-gnu-gcc

The first hardware gate uses the existing
`mt6797-gemini-pda-jpeg.dtbo` overlay, but submits no jobs.  Do not compose a
display or other media-consumer overlay into this isolated gate.  Before
opening any video node, run:

    capture-jpeg-preflight.sh

Proceed only when it prints `JPEG_PREFLIGHT=PASS`.  This verifies exact driver
and video-node binding, IOMMU attachment, stable registered IRQs, suspended
runtime-PM state, disabled clocks, and powered-off MM/VENC domains.  It also
rejects accidental GCE activation, probe errors, and IOMMU faults.

For the subsequent one-job gate, identify the encoder and decoder paths from
`v4l2-ctl --list-devices`, then run exactly once:

    gemini-jpeg-qual --encoder /dev/video0 --decoder /dev/video1 --timeout 3000

The device paths are mandatory to avoid accidentally testing unrelated video
nodes.  The timeout is per wait and must be between 100 and 30000 ms.  A pass
prints the encoded byte count and a checksum for every decoded plane.  Any
format mismatch, timeout, buffer error, malformed JPEG, or short decode is a
failure.  On every exit after queue setup, the tool stream-offs active queues,
unmaps buffers, releases MMAP buffers, and closes both devices.
