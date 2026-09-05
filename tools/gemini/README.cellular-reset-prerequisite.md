# MT6797 cellular reset prerequisite

This change fixes a concrete probe prerequisite, not a cellular activation.
The control owner requests `cldma-pdn` and `cldma-ao` exclusive resets before
its power/transport checks. The compact reset provider initially exposed only
thermal/PWRAP IDs0/1 and rejected the disabled modem DT's historical IDs13/68.

Exact vendor evidence, inspected locally in
`/home/grw/src/gemini-linux-kernel-3.18/drivers/misc/mediatek/eccci/mt6797/`:

- `cldma_reg.h:19-24`: PD SET0x120/CLR0x124, bit13; AO SET0x140/CLR0x144,
  bit4. SHA256 `a6f0119e5db899aa0657f95794f703fd8f00b3fbc0889325d87cdd17938bb32b`.
- `cldma_platform.c:69-98`, `md_cldma_hw_reset()`: uses those AO and PD
  assert/deassert pairs. SHA256
  `ada25c673de99cf648955cc782fabf7ab98af1c6bf3227ca7883d5ff44792125`.

Compact public IDs2/3 map to internal bank0 bit13 and bank1 bit4. No additional
reset bank is invented. Both SET and CLR addresses/masks are covered by the
actual descriptor/translation KUnit tests, run by `test-thermal-acquisition.py`.
IDs4,13,64,68 and internal bank overflow remain rejected.

The existing symbolic MD1 DTS references automatically resolve to IDs2/3.
`remoteproc@10014000` remains disabled. Old external carrier DTs must translate
their MD1 references before activation; this change does not modify such blobs.
It makes no claims about safe transport ownership, reset sequencing, firmware
handshake, SIM/network registration or actual hardware reset operation.

Battery repair is not a power prerequisite: the user confirms power is fine.
Battery temperature remains physically unavailable with only + and - wired.
An absent explicit request_firmware package is also not proof the LK-handoff
path is blocked: the retained Sep1 board capture verifies an LK-loaded MD1 v5
header and post-vector status0x54430001, but that diagnostic used degraded PLL
continuation and installed no CLDMA/CCCI transport. Neither is transport proof.
