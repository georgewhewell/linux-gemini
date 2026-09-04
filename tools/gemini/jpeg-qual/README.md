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

Save the passing preflight output.  For the subsequent one-job gate, identify
the encoder and decoder paths it reported, then run exactly once:

    gemini-jpeg-qual --encoder /dev/video0 --decoder /dev/video1 --timeout 3000

The device paths are mandatory to avoid accidentally testing unrelated video
nodes.  The tool also resolves each character device through sysfs and requires
the exact MT6797 encoder or decoder platform ancestor.  The timeout is per wait
and must be between 100 and 30000 ms.  A pass prints the encoded byte count and
an active-pixel checksum for every decoded plane.  Any format mismatch,
timeout, buffer error, malformed JPEG, constant/short decode plane, or cleanup
error is a failure.  On every exit after queue setup, the tool attempts to
stream-off active queues, unmap every mapped plane, release MMAP buffers, and
close both devices.

After the command exits, run `capture-jpeg-preflight.sh` again.  Accept J1 only
if both captures and the qualification command pass, the encoder and decoder
IRQ totals each increased by exactly one between captures, the M4U IRQ total
did not change, and the second capture shows every device suspended with the
MM/VENC domains off and all target clock counts zero.  Stop on the first failed
command, unexpected IRQ delta, timeout, IOMMU fault, or failure to return to the
preflight idle state; the next RAM boot must omit the JPEG overlay.
