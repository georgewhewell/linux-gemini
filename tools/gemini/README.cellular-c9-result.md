# C9: storage-disabled package refused before modem start

Candidate `2c2304ff4c73b670e09ac8d6d3d0897336865573ff62480a22a1f1b181f21eba`
was written once, boot `2fe83403-3f2f-47e9-a38a-4e131e700332`.
Evidence lives in `handoff/cellular-c9-oGixgWwZ` in the workspace.
This candidate is consumed; do not retry it.

The bundled launcher ran once with410 seconds remaining in the unchanged
original conservative deadline. Prepare failed with ENOENT; its final log says
`MD1 FS authority registration failed: No such file or directory; no retry`.
No test directory or mountinfo-mounted capture was created. Final mountinfo
contains only rootfs/devtmpfs/proc/sysfs/debugfs/tmpfs, not nvdata. The modem
cold owner remains deferred waiting for scoped filesystem authority.

The actual compiled kernel.config at
`/nix/store/fj81czk0i7iapmdf0rngssfr5lbgj45j-linux-gemini-cellular-oneboot-c9-7.1.3/kernel.config`
line2921 has `# CONFIG_MMC is not set`; EXT4 is built in. Candidate dmesg has
no MMC enumeration. The helper's first operation opens `/dev/mmcblk0p6`;
without MMC this device cannot exist. The package retained C8's storage-disabled
profile while introducing a dependency on real eMMC backing. Existing host
fixtures mocked block-device availability and did not catch that omission.

Thus this is a package dependency failure before the mount, REGISTER or modem
start. It is not evidence that the real backing is absent, that the modem
rejected GETDISKINFO, or that HS2/AT failed after a response. No timer or protocol
change follows from it. Fix the successor's explicit MMC/block/host dependencies
and effective DT using the established bounded eMMC profile, with actual built
config/DT checks and a negative fixture for the C9 omission. Add stage-specific
helper errors so precheck failures do not misleadingly claim registration ran.

Owner captured and fsynced preparation RAM archive SHA256
`9ca7948474c45c7bb5b690c2c54c9d66b416d465ce1ce8c87e59eac8a37e9508`
before stopping verified petter110. Original execution93077 and supervisor
retain recovery ownership. Full rescue/hash/network/pstore/process closure is
pending at this note; do not authorize another transaction from this result.
