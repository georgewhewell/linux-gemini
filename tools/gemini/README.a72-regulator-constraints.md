# A72 regulator DT constraints

The custom DA9213 and secure SRAM providers previously supplied config.of_node
without config.init_data or descriptor.of_match. The regulator core therefore
associated the DT nodes but did not load their names, voltage bounds or
operation permissions. M7 showed descriptor names and zero bounds; a disabled
BUCKB could then fail regulator_enable with EPERM.

Both providers now parse their exact selected nodes with
of_get_regulator_init_data and pass the result to registration. A failed parse
refuses registration. The DA9213 path releases child and parent references on
failure and skips disabled nodes before parsing. The helper keeps its exact
DT-provider/supply identity checks and now expects the parsed DT names.

This enables the existing DT policy, so it is not merely a display-name fix.
On Gemini the policies are BUCKA770000..1200000uV/always-on,
BUCKB800000..1200000uV, and SRAM600000..1200000uV. The core may bring an
out-of-range voltage into bounds; with observed1000/1000/1050mV this is not
needed. The core calls BUCKA enable for always-on; the DA9213 transaction
returns without a write when the enable bit already equals the requested bit.
BUCKB and SRAM lack always-on/boot-on and are not enabled just by registration.
No unsupported SRAM enable/disable service is promoted to a working operation.
The ordinary SRAM voltage setter still refuses initial selector0 because it
cannot restore it; first-start one-way initialization remains separate work.

Host tests run actual registration functions against DT/core boundary doubles
and check exact node/descriptor/parsed-data identity, parsing refusal and node
cleanup. ARM64 object compilation checks kernel API integration. Neither test
proves real regulator-core startup effects or hardware operation; those remain
part of the complete next-candidate review and supervised validation.
