# V4 offset rounding and calibration mapping correction

Adapted from ixoo/gemini-pda-mainline commit
57c07ecb89c6c8401da1946a85c55547216de04f, patch0541, archived kernel commit
0e14956f061709511948e08afd642971d280216d. That archive identifies
Gemini Mainline Experiment <gemini-mainline@example.invalid> as a non-certifying
experimental author and contains no DCO sign-off; none is invented here.
The MIT actual-C test derives from test-v4-correction.py introduced at
7d8688815e8925258eb3cad635a94e5c6322645e. Original project author metadata is
Julien Etienne <julien.etienne@gmail.com>; local fixture adaptation is distinct.

Normalize the encoded OE field by512 before separately rounding both conversion
terms. Exact-real-number cancellation does not justify omitting normalization
under integer rounding. Vendor8cfe6596 mtk_tc.c:491 and495–499 independently
corroborate normalization. The synthetic GE/OE265, degree1, VTS0, slope0,
raw2926 golden is55600mC, not the predecessor's55700mC. This is not an adjustment
computed for our captured device temperatures or proof of physical accuracy.

Validate sensor IDs before indexing and use the existing VTS map. ABB sensor4
maps to calibration index5 rather than unused VTS5 index4. No bank, mux or ABB
acquisition is enabled. Production changes are restricted to raw_to_mcelsius_v4;
low12-bit masking, aggregate EAGAIN, legitimate0C, four-channel hwmon, acquisition,
reader lifetime and other SoC converters remain unchanged. No0542 configuration
identity, DT, package or hardware change is included.

Run from the source root:

```
python3 tools/gemini/test-thermal-v4-offset.py --source drivers/thermal/mediatek/auxadc_thermal.c
python3 tools/gemini/test-thermal-mt6797-unready.py
python3 tools/gemini/test-thermal-physical-hwmon.py
python3 tools/gemini/test-thermal-acquisition.py
```

CC selects the host compiler. The first suite extracts the actual production
decoder/converter, enum and calibration map without modifying source. It tests
177 decoder cases,3,316,950 grid conversions,6,633,900 upper-mask comparisons,
81,900 distinct-sensor cases, an explicit rounding golden and10 rejected
mutants under undefined-behavior/bounds sanitizers. Invalid sensor IDs, invalid
fuses, ID0, both slope signs, full nonzero12-bit range and zero/flag-only refusal
are covered. The existing aggregate suite's fixture now carries the real map;
it still finds a legitimate0C at raw3754 and exercises actual bank/zone code.
These are host tests, not kernel KUnit or device observations.

ARM64 GCC12 compiled the changed translation unit using the existing prepared
T8 generated headers/config (SHA256
ae71cdf53007abff1a2885365dd19b9c75a0438b7d49a2fdccc8af1d6f4ca7d7), with
all source include paths and per-object names redirected to this worktree.
No prepared baseline object or config was overwritten. This is an object check,
not a new linked image or authorization for another thermal boot.
