# Gemini touchscreen contract evidence collector

`tools/gemini/touch-vtouch-preflight.sh` is a vendor-boot **read-only**
collector. It reads the flattened device tree plus already-present regulator,
GPIO, pinctrl, and optional already-captured vendor-log reports. It does not
open `/dev/i2c-*`, load a module, request/toggle GPIOs, enable/disable a
regulator, issue an identity read, or write sysfs, debugfs, or devmem.

Run it on the vendor kernel and retain stdout with the boot/log identity:

```
tools/gemini/touch-vtouch-preflight.sh > /persistent/path/touch-contract-evidence.txt
```

To retain a vendor-driver identity already present in a saved kernel log:

```
KERNEL_LOG=/persistent/path/vendor-dmesg.txt \
  tools/gemini/touch-vtouch-preflight.sh > /persistent/path/touch-contract-evidence.txt
```

The output is line-oriented. `schema=gemini-touch-contract-evidence-v3` identifies the
schema. Values ending in `_phandle`, `_compatible`, `_pins`, or `_uv` are raw
DT bytes encoded as lowercase hexadecimal. Values of `unknown`, `unresolved`,
`unavailable`, and `none` are intentional evidence states, not defaults.
`*_provider=` identifies the DT node whose `phandle` or `linux,phandle`
byte-for-byte matches the corresponding consumer property.

The collector records source-derived expectations separately from live facts:
`vendor_touch_vdd_expected_provider=mt6351_vldo28`,
`vendor_touch_vdd_expected_uv=2800000`, `vendor_irq_expected_gpio=85`,
`vendor_irq_expected_function=EINT8`, and `vendor_reset_expected_gpio=68`.
GPIO68 is explicitly a snapshot only: it cannot prove reset polarity or
timing.  `identity_read_by_collector=no` makes clear that any retained identity
came from the existing vendor log, not a new I2C transaction.

Local vendor sources support only these starting facts:

- `arch/arm64/boot/dts/mt6797.dtsi:4041-4043` defines the separate `touch@`
  pseudo-node (`compatible = "mediatek,mt6797-touch"`); its
  `aeon6797_6m_n.dts:579-598` consumer names six pinctrl states.
- `aeon6797_6m_n.dts:603-623` maps GPIO85 between EINT8 and GPIO output states;
  it literally names `PINMUX_GPIO85__FUNC_EINT8`. `aeon6797_6m_n.dts:624-637`
  names `PINMUX_GPIO68__FUNC_GPIO68` with both output-low and output-high.
- `novatek-nt36xxx-i2c.dtsi:19-25` describes the separate I2C4 `cap_touch@62`
  client; the alternate `0x01` protocol target is not a second client.
- `drivers/misc/mediatek/dws/mt6797/aeon6797_6m_n.dws:3848-3871` identifies the
  board PMIC as MT6351 and maps `CAP_TOUCH_VDD` to `VLDO28`.  Mainline exposes
  it as the fixed 2.8 V `ldo-vldo28` rail in
  `drivers/regulator/mt6351-regulator.c:321`.
- `aeon6797_6m_n.dts:624-637` only defines both GPIO68 output states; the NVT
  driver selects GPIO85 at `aeon_nt36xxx/nt36xxx.c:1392-1403` and does not
  establish a GPIO68 sequence.
- `README.touchscreen-identity.md` documents the separate, recovery-only
  stateful identity overlay.  Do not use it until this collector observes
  VLDO28 and confirms that no vendor client owns I2C4/0x62.

Run the fixture test without hardware:

```
tools/gemini/test-touch-vtouch-preflight.sh
```
