# MT6797 VDEC mainline qualification

This note records why MT6797 video decode must remain unbound for now.  It is
not an activation recipe.  Run the adjacent contract test against the immutable
vendor source used for this comparison:

```
tools/gemini/test-mt6797-vdec-contract.sh \
        /mnt/Home/src/gemini-vendor-kernel-3.18
```

## Evidence baseline

The vendor reference is commit
`8cfe6596a503612e3332d9c26e292a19525a7f07` (tree
`3d7edd04f9edb6d88b93468a659f1cf11a12b8ae`).  The contract test pins the
content hashes of every vendor file on which the conclusion depends, so a
different worktree cannot silently pass.

The MT6797 infrastructure already described by the mainline-style tree is
consistent with the vendor tree:

* `vdecsys` covers `0x16000000..0x1600ffff` and provides the VDEC gates.
  `CLK_VDEC_CKEN` is the engine gate and `CLK_VDEC_LARB1_CKEN` is the larb
  gate.  The VDEC power domain owns the top-level `vdec` mux.
* `larb1` covers `0x16010000..0x1601ffff`, belongs to the VDEC power domain,
  and is disabled.  Its ten M4U ports (MC through TILE) exactly preserve the
  vendor larb/port numbers 0 through 9.
* `m4u`, `smi_common`, and `larb1` are all disabled.  There is therefore no
  accidental consumer activation in the base DT.

Those facts qualify the provider side only.  They do not qualify a V4L2 VDEC
consumer.

## Register-map incompatibility

The legacy upstream binding and `mtk_vcodec_get_reg_bases()` require exactly
these eleven named windows.  The MT6797 vendor DT supplies a different tail:

| upstream name | upstream/MT8173 address | MT6797 vendor evidence |
| --- | ---: | --- |
| `misc` | `0x16020000` | `vdec`, `0x10000` aggregate |
| `ld` | `0x16021000` | `vld`, `0x800` |
| `top` | `0x16021800` | `vld_top`, `0x800` |
| `cm` | `0x16022000` | `mc`, `0x1000` |
| `ad` | `0x16023000` | `avc_vld`, `0x1000` |
| `av` | `0x16024000` | `avc_mv`, `0x1000` |
| `pp` | `0x16025000` | `vdec_pp`, `0x1000` |
| `hwd` | `0x16026800` | `vp8_vld`, `0x800` |
| `hwq` | `0x16027000` | absent |
| `hwb` | `0x16027800` | `vld2`, `0x800` |
| `hwg` | `0x16028400` | absent |
| no legacy name | none | `hevc_vld`, `0x16028000`, `0x1000` |

Aliasing MT6797 to `mediatek,mt8173-vcodec-dec` would consequently map two
unproven register windows and misdescribe HEVC.  The fact that the vendor's
aggregate `vdec` mapping spans all these addresses is not evidence that the
missing blocks implement the MT8173 register ABI.

## Firmware ABI incompatibility

The current decoder probe rejects a node without either `mediatek,vpu` or
`mediatek,scp`.  The only in-tree VPU provider is
`mediatek,mt8173-vpu`; it boots MT8173 `vpu_p.bin`/`vpu_d.bin` images and owns
the MT8173 TCM/IPI ABI.

MT6797 has no VPU or SCP provider node in this tree.  The immutable vendor
implementation instead exposes `/dev/vpud` and transports codec objects via
the `VPUD_SET_OBJECT` userspace ioctl.  Its actual MT6797 codec driver is the
separate misc driver matched by `mediatek,mt6797-vdec_gcon`; the nominal vendor
V4L2 driver does not provide an MT6797 OF match.  A phandle to the MT8173 VPU,
or a fabricated SCP node, would therefore be an ABI claim without evidence.

## Closed activation gate and next code seam

Do not add an MT6797 VDEC compatible, consumer node, or Gemini activation
overlay until both of these gates are met:

1. A standard kernel firmware provider for MT6797 is documented and
   implemented from redistributable firmware and an evidenced register, IRQ,
   memory, watchdog, and IPI/shared-memory ABI.  Its driver must complete a
   bounded boot and IPI exchange independently of the codec consumer.
2. An MT6797-specific decoder platform-data entry and register layout are
   implemented.  Its codec set and interrupt/status accesses must be checked
   against MT6797 evidence; it must not reuse the MT8173 data by compatible
   alias.

Only then should a schema conditional be added, followed by a disabled SoC
node and a Gemini RAM-only overlay that enables, in dependency order,
`m4u`, `smi_common`, `larb1`, the proven firmware provider, and VDEC.  The
first activation experiment must reject on provider firmware/IPI failure
before opening `/dev/video*`; successful probe and a bounded V4L2 decode job
are separate gates.

An upstream stateless V4L2 backend that programs MT6797 directly could replace
the firmware-provider step, but that is a new codec backend, not a compatible
or DT-only patch.
