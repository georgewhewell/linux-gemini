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

After booting the RAM-only JPEG activation overlay, identify the encoder and
decoder paths from `v4l2-ctl --list-devices`, then run:

    gemini-jpeg-qual --encoder /dev/video0 --decoder /dev/video1 --timeout 3000

The device paths are mandatory to avoid accidentally testing unrelated video
nodes.  The timeout is per wait and must be between 100 and 30000 ms.  A pass
prints the encoded byte count and a checksum for every decoded plane.  Any
format mismatch, timeout, buffer error, malformed JPEG, or short decode is a
failure.  On every exit after queue setup, the tool stream-offs active queues,
unmaps buffers, releases MMAP buffers, and closes both devices.
