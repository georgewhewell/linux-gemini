# MT6797 RF clock RPC

CI4 boot `7949df63-5933-4cbc-9e9f-e3f384bdb836` exposes an unanswered
`0x4008` RPC on channel32/queue1, 36 bytes, in `rx_rejected` before any RF
activation. The FS worker is idle, ready and healthy at that observation.
This is an actual missing operation, not yet proof of the SIM-busy cause.

## Vendor evidence

Reference: `gemini-vendor-kernel-3.18` commit
`8cfe6596a503612e3332d9c26e292a19525a7f07`.

* `drivers/misc/mediatek/eccci/mt6797/ccci_config.h` enables both
  `FEATURE_RF_CLK_BUF` and `MD_UMOLY_EE_SUPPORT`.
* `eccci/port_rpc.h` defines `IPC_RPC_GET_RF_CLK_BUF_OP=0x4008`, a packed
  six-byte input (u16 count, unaligned u32 AFC word), and packed 18-byte
  result (u16 count, four configuration bytes, four software-control bytes,
  four u16 driving values). `port_rpc.c:rpc_msg_handler` requires one
  argument. Normal count is4; other counts return sentinel0xff.
* The generic serializer rounds argument payloads to four bytes. A successful
  reply is56 bytes: 16-byte CCCI header, op/count, u32 status argument,
  18-byte result argument padded to20. Parameter errors use two u32 error
  arguments, total40 bytes. Input padding need not be zero.
* Actual saved vendor config has `CONFIG_ARCH_MT6797=y` and no
  `CONFIG_MTK_LEGACY`. `base/power/Makefile` selects the platform directory;
  `base/power/mt6797/Makefile` builds its `mt_clkbuf_ctl.o`.
* In that MT6797 header, `ENABLE_PMIC_CLK_BUFFER` is commented out.
  `is_clk_buf_from_pmic()` therefore returns false without touching PMIC
  debug registers. The PMIC branch's zero result is **not** this configuration.
* `arch/arm64/boot/dts/mt6797.dtsi` supplies RF configuration `<2 0 1 1>`
  and driving `<2 2 2 2>`. The effective board DT still needs explicit
  verification before using these as the runtime provider's metadata.
* The driver's enabled `TEST_SUGGEST_RF_DRIVING_CURR_BEFORE_MP` overrides
  all returned RF driving values to `CLK_BUF_DRIVING_CURR_0_9MA`, enum1.
  Reporting the DT's enum2 would not reproduce this vendor build.
* `clk_buf_get_swctrl_status()` reports the modem-on vector `<1 0 1 1>`
  when not in flight mode. Flight mode uses a separate mutable vector;
  these are software state, not hardcoded proof of present clock ownership.
* `clk_buf_save_afc_val()` retains every received AFC word and writes the
  first one to `SPM_BSI_EN_SR`. Subsequent calls update retained state only.
  `clk_buf_write_afcdac()` writes that retained word during suspend.
  `spm_v2/mt_spm_reg_mt6797.h` identifies this full-width register at SPM
  offset0x414, physical0x10006414. Initialization also writes default
  `0x02a80008` to that register. AFC must not be printed in diagnostic logs.

The current mainline modem prepower owner handles MDBSI_CON offset0x404,
not BSI_EN_SR0x414. No matching mainline MT6797 owner was found for0x414 in
the current soc/clock/cpuidle/CCCI drivers. This search does not substitute
for validating provider ownership and lifecycle.

## Implemented codec and remaining integration

`mtk_mt6797_ccci_rpc_clkbuf.h` validates the whole request before invoking
a supplied process-context provider. A valid request calls the provider
once with the correctly unpacked AFC word. There are no default clock
values and no hardware access in the codec. A failed provider or invalid
returned state produces vendor `FS_FUNC_FAIL=-5`, never success. Invalid
count returns the vendor sentinel with initialized unused bytes and no
provider call. Structurally malformed packets never invoke the provider.

The header is not yet connected to CCCI RX. A bounded worker and a real
clock provider are required; NAPI cannot call a sleeping regmap provider.
The implementation must account for the two RPC request slots, teardown,
fatal state, failed TX after AFC publication, and first-update semantics.
It must also retain AFC for future suspend integration. Wire success alone
must not be used as evidence of a complete clock service or radio attach.

`test-cellular-rpc-clkbuf.py` compiles the actual codec with a counted
provider. It compares a literal56-byte response, all65535 invalid counts,
every input truncation and insufficient output capacity, malformed length,
overflow, trailing bytes, semantic error shapes, both slots, provider
failure and invalid returned values. No frame payload is from provisioning.
