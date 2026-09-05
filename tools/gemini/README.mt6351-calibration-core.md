# MT6351 pure calibration core

This is host-tested arithmetic, not a registered driver or a live temperature
reading. `drivers/thermal/mediatek/mt6351_calibration.h` has no hardware or
registration operations and is not included by any active driver. Acquisition,
OTP engine ownership and safe timeout disposition remain unresolved as recorded
in `README.mt6351-die-temperature.md` and
`README.mt6351-pmic-temperature-path.md`. Battery NTC is disconnected; this
code is solely for the independent PMIC die sensor, not battery policy.

The caller must acquire exactly two genuine thermal OTP words before decode;
this core does not supply, cache or invent them. Decode returns `-ENODATA`
for calibration-enable clear and `-ERANGE` for calibration temperature outside
38..60. ID=0 forces slope=0 as specified by the vendor. Reserved high bits of
word1 are ignored, not interpreted as new calibration fields. Conversion
requires decoded calibration and a 12-bit ADC code, not a flag-bearing result
register or already scaled millivolts. All public outputs remain unchanged on
error. A real zero-degree result is valid; raw zero is an arithmetic input,
not evidence that an ADC conversion was ready. Readiness belongs to acquisition.

Vendor source is pinned in the header. The unusual distinct slope factors
are retained, as are integer-millivolt truncation and signed division toward
zero. Intermediates are signed64 with checked multiplication/addition. Accepted
field bounds make overflow unreachable with genuine decoded input, while
tests also exercise the underlying arithmetic guard with signed64 extremes.
Temperature outside -50000..150000 milli-Celsius is rejected, not replaced
with a previous reading. These vendor bounds are not a charging safety policy.

Run:

```
CC=gcc python3 tools/gemini/test-mt6351-calibration.py
```

The test compiles the actual header with minimal Linux type/math/overflow
adapters and UBSan, with `-Wall -Wextra -Werror`. It checks independent literal
golden results (including the exact integer operands in comments), both slope
signs, ID0, all VTS encodings, all calibration-temperature encodings, malformed
decoded fields, raw extremes, genuine zero, untouched error outputs, signed
division and overflow. This validates arithmetic, not the omitted hardware
transaction or firmware exclusion.
