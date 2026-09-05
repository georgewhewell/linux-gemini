# C1 failure and live shared-memory resolution

C1 candidate `8674616e5d1863a464111cfdc3eea6a13b82a79e093ba9037586bd82ce6c8071`
is consumed. It reached SSH but failed before HS1/HS2; no AT command was sent.
The original transaction closed at 03:56:49 on 2026-09-05 with exact rescue,
pstore, all partition readbacks and USB/RJ45 health verified. Recovered boot:
`ccc3188b-7d93-48a3-b332-0ef91fe03b15`.

Saved handoff: `handoff/cellular-c1-CF4xk7eS`.
RAM SHA256: `f9c353a4a7a2892c60d459cf637c6cbe324ca9deb9c1f9b850b65abd6af21593`.
Live DT SHA256: `96f3545a2410c03db4a1bd5835a90d9caf518e5e706366c20b4c9be213fc1e94`.
Pstore SHA256: `3804dcc1a0c333617137cf2870cda29208c068326eb6fed3b8044573d60966b8`.

## Concrete failures

CCCI returned -EINVAL at 0.968 seconds without its resource-topology or
lifecycle error message. The built modem node references memory-region 0x101;
the live modem retains that reference, but LK has deleted its target
`reserve-memory-ccci_share` and installed `mblock-5-ccci` instead. The old
of_parse_phandle call therefore fails before lifecycle admission. This is not
an IRQ, DMA-ring or modem handshake failure.

Separately, the built configuration disables MTK_PMIC_WRAP and MFD_MT6397.
The cold owner correctly defers with MT6351 PMIC regmap unavailable. This
requires a separately reviewed package/provider fix, not bypassed admission.

## Source change

CCCI now derives its mapping resource from the published LK handoff layout.
That provider already validates all three live CCCI mblocks, exact ranges,
no-map attributes, firmware/plan and overlap exclusions before publication;
it explicitly rejects surviving pre-LK CCCI reservation nodes. Requiring a
pre-LK phandle after that contract was contradictory.

The consumer still requires GO, completed passive observation, published
layout and plan, and its exact carveout validator before mapping shared
memory. No static reg, reservation, alternate physical address or new memory
ownership is invented. The resource and runtime configuration are output
only after all checks pass. A subtraction-underflow check prevents a malformed
AP-to-MD offset from wrapping into an apparently valid 32-bit address.
The subsequent single CLDMA mapping/reset lease remains unchanged.

Actual handoff and carveout C functions pass one success and 25 refusal
fixtures, including provider errors, missing completion, unvalidated load,
range mismatch and address underflow. Refusals leave output untouched.
An independent agent reviewed the contract and reran those tests. ARM64
mtk_mt6797_ccci.o compilation passed in
`/tmp/gemini-cellular-live-smem-build-SSOwUM1X`.
These checks do not establish modem startup; a fresh reviewed successor is
required for hardware validation.
