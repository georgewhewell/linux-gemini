# Gemini VTOUCH evidence collector

`tools/gemini/touch-vtouch-preflight.sh` is a vendor-boot **read-only**
collector. It reads the flattened device tree plus already-present regulator,
GPIO, and pinctrl debugfs/sysfs reports. It does not open `/dev/i2c-*`, load a
module, request/toggle GPIOs, enable/disable a regulator, or write sysfs,
debugfs, or devmem.

Run it on the vendor kernel and retain stdout with the boot/log identity:

```
tools/gemini/touch-vtouch-preflight.sh > /persistent/path/vtouch-evidence.txt
```

The output is line-oriented. `schema=gemini-vtouch-evidence-v2` identifies the
schema. Values ending in `_phandle`, `_compatible`, `_pins`, or `_uv` are raw
DT bytes encoded as lowercase hexadecimal. Values of `unknown`, `unresolved`,
`unavailable`, and `none` are intentional evidence states, not defaults.
`*_provider=` identifies the DT node whose `phandle` or `linux,phandle`
byte-for-byte matches the corresponding consumer property.

The collector records source-derived expectations separately from live facts:
`vendor_irq_expected_gpio=85`, `vendor_irq_expected_function=EINT8`, and
`vendor_reset_expected_gpio=68`. It leaves reset polarity and timing as
`unknown`; the collector never changes the reset line. A successful provider
match is still not permission to enable a mainline touchscreen client: retain
the final `activation_gate` until voltage, reset polarity/timing, and recovery
procedure have been reviewed.

Local vendor sources support only these starting facts:

- `arch/arm64/boot/dts/mt6797.dtsi:4041-4043` defines the separate `touch@`
  pseudo-node (`compatible = "mediatek,mt6797-touch"`); its
  `aeon6797_6m_n.dts:579-598` consumer names six pinctrl states.
- `aeon6797_6m_n.dts:603-623` maps GPIO85 between EINT8 and GPIO output states;
  it literally names `PINMUX_GPIO85__FUNC_EINT8`. `aeon6797_6m_n.dts:624-637`
  names `PINMUX_GPIO68__FUNC_GPIO68` with both output-low and output-high.
- `novatek-nt36xxx-i2c.dtsi:19-25` describes the separate I2C4 `cap_touch@62`
  client. It does not supply a VTOUCH or reset contract.
- `mt6335.dtsi:221-226` mentions a 2.8 V regulator named `vtouch`, but
  `aeon6797_6m_n.dts:3` includes `mt6797.dtsi`, not `mt6335.dtsi`; therefore it
  is not evidence that this Gemini's live VTOUCH provider is MT6335 or 2.8 V.

Run the fixture test without hardware:

```
tools/gemini/test-touch-vtouch-preflight.sh
```
