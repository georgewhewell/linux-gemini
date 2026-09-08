# Real tmpfs provider integration test

Run the production provider in User Mode Linux:

```
python3 tools/testing/kunit/kunit.py run --arch=um \
  --kunitconfig=tools/gemini/cellular-fs-vfs.kunitconfig \
  --build_dir=/absolute/path/to/separate-uml-build \
  'mt6797-md1-fs-vfs*'
```

A working native kernel build environment is required. The fragment enables
SMP to accommodate this tree's CPU rollback instrumentation, disables compiler
plugins, and uses the existing cellular test policy for WERROR. The generic
rollback publication hooks now have weak no-op defaults for architectures
without observers; ARM64's strong implementations remain in place.

The explicit UML-only test option compiles a private fixture into the provider
translation unit. It creates three separate bounded tmpfs mounts, publishes
those roots as consumed test authority, invokes production operations, and
cleans up all retained files/mounts. It does not register a cold owner or use
any modem, DT platform device, block partition, backup region or live phone.
The phone's ARM64 build cannot enable this UML-only test option.

Verified against actual VFS/tmpfs (one KUnit case with multiple assertions):

- Creation on Z/X/Y, duplicate mkdir, attributes, distinct file contents,
  non-empty-directory refusal, rename and lookup of old paths.
- Cross-mount rename refusal, directory ancestor trap, same-name rename,
  root/dot-dot refusal and replacement of an existing destination inode.
- Open descriptor readability after rename and unlink; absence by name after
  unlink; delayed inode lifetime while its descriptor remains open.
- Live free space decreasing after 128 KiB of writes, remaining allocated
  after unlink with an open descriptor, and recovering after final close.
- Empty-directory removal after its contents have moved away.

This complements the instrumented tests of resource failure paths. It does
not qualify concurrent hostile namespace changes, every confinement class,
wire framing, registration/cold-owner admission, or execution on ARM64/modem
hardware. Those require their respective tests. MOVE, RESTORE, XDELETE and
persistent provisioning writeback are still separate unfinished work.
