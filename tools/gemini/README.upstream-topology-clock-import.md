# Narrow topology and clock-decoder import, 2026-09-05

Imported unchanged with `git am` from ixoo's reproducibility repository at
`b7b72311c5eecfbead4b45494e2dd2f467d559b7`, `patches/v7.1.3/`:

| Patch | Original format-patch commit | Scope |
| --- | --- | --- |
| 0482 | 8b6b2e50813f1a7895866f76f0b78a02dd0f9080 | Declarative 4+4+2 CPU map |
| 0525 | 3223c4c9ef08487c142046b1b104ddb44fefa7cc | Normal versus BigiDVFS PLL decoding |
| 0526 | 44b3318e1aa17b30d3dbd31ffeddd1e913ca2d63 | Pure decoder KUnit tests |
| 0533 | 63e691c16ae3a0d1f8d408db822dd283280d4198 | Selector zero identity ratio |
| 0534 | 702d9e318b561d2165e73e6b5b048697702ff899 | Live tuple regression test |

The original author (including its explicit internal-experiment identity),
dates and messages are preserved. No DCO certification is added or implied.
Local integration parent: `4d478e29c434840ca20b7cab0125184cc914527a`.

The originating exact runtime evidence is
`experiments/2026-09-04-mt6797-a72-frequency-observation/results/zero-divider-runtime-pass-20260904.txt`.
That result used repository build revision
`556575a202e09d25093c578cded454854a3e6d08` and a much larger, separately
configured admission chain. It is not hardware validation of this import.

## Local validation

- `HOSTCC=<host-gcc> python3 tools/gemini/test-mt6797-clock-decoder-host.py`:
  all seven unchanged KUnit case bodies pass, 59 assertions, against the
  complete actual decoder and actual public record definitions. Only kernel
  primitive/assertion plumbing is substituted. This is host execution, not
  a KUnit kernel boot.
- Both `mt6797-dvfsp-clock-state.o` and
  `mt6797-dvfsp-clock-state-test.o` cross-compile for arm64 with GCC 12.2.0,
  `CONFIG_KUNIT=y`, `CONFIG_MTK_MT6797_DVFSP_STATE_DECODERS=y` and the new
  test option enabled. An initial host-tool PATH omission stopped DTC
  generation; supplying Flex/Bison fixed the environment and the build passed.
- Preprocessed the actual `mt6797-gemini-pda.dts` with GCC and compiled it
  with DTC 1.7.2. Checked all ten compiled cpu-map phandles against the
  matching CPU nodes: clusters 0/1/2 contain CPUs 0-3/4-7/8-9 respectively.
  The existing two USB empty-ranges warnings remain; no new DT warning.
- `git diff 4d478e29c4348 --check`: pass.

No hardware, CPU request, clock write, policy enablement, partial admission
chain, default A72 support, or new boot artifact is part of this change.
The independent CPU-clock provider implementation is not replaced here.
