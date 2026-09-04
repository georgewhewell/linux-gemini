#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Structural regression check for the MT6797 single-owner CSPM contract.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cpuclk="$root/drivers/clk/mediatek/clk-mt6797-cpu.c"
handoff="$root/drivers/soc/mediatek/mt6797-dvfsp-handoff.c"
access="$root/drivers/soc/mediatek/mt6797-dvfsp-cspm-access.c"
access_test="$root/drivers/soc/mediatek/mt6797-dvfsp-cspm-access-test.c"
i2c="$root/drivers/i2c/busses/i2c-mt65xx.c"
handoff_header="$root/include/linux/soc/mediatek/mt6797-dvfsp-handoff.h"
dtsi="$root/arch/arm64/boot/dts/mediatek/mt6797.dtsi"
binding="$root/Documentation/devicetree/bindings/clock/mediatek,mt6797-mcumixedsys.yaml"

require_fixed()
{
	needle=$1
	file=$2
	if ! grep -F -q -- "$needle" "$file"; then
		echo "missing single-owner CSPM invariant in $file: $needle" >&2
		exit 1
	fi
}

reject_fixed()
{
	needle=$1
	file=$2
	if grep -F -q -- "$needle" "$file"; then
		echo "obsolete second-owner CSPM pattern in $file: $needle" >&2
		exit 1
	fi
}

# The handoff is the DT's only owner of the physical CSPM window. CPUCLK is
# an access-controller consumer and has only its own MCUMIXED/MCUCFG ranges.
require_fixed 'dvfsp-handoff@11015000' "$dtsi"
require_fixed '#access-controller-cells = <0>;' "$dtsi"
require_fixed 'access-controllers = <&dvfsp_handoff>;' "$dtsi"
require_fixed 'reg-names = "mcumixed", "mcucfg";' "$dtsi"
if [ "$(grep -F -c -- '0x11015000' "$dtsi")" -ne 1 ]; then
	echo 'MT6797 DT must describe exactly one CSPM owner at 0x11015000' >&2
	exit 1
fi

require_fixed 'access-controllers:' "$binding"
reject_fixed 'const: cspm' "$binding"

# CPUCLK must acquire and validate its supplier before the probe snapshot. It
# receives the CSPM base only inside the owner-controlled callback and never
# maps or retains the shared resource itself.
require_fixed 'observer->handoff = mt6797_dvfsp_handoff_get(&pdev->dev);' "$cpuclk"
require_fixed 'mt6797_dvfsp_handoff_require_ready(observer->handoff);' "$cpuclk"
require_fixed 'mt6797_dvfsp_cspm_transaction(' "$cpuclk"
reject_fixed 'platform_get_resource_byname(pdev, IORESOURCE_MEM, "cspm")' "$cpuclk"
reject_fixed 'devm_ioremap(&pdev->dev, cspm->start' "$cpuclk"
reject_fixed 'observer->cspm' "$cpuclk"

owner_line=$(grep -n -F \
	'observer->handoff = mt6797_dvfsp_handoff_get(&pdev->dev);' \
	"$cpuclk" | sed -n '1s/:.*//p')
ready_line=$(grep -n -F \
	'mt6797_dvfsp_handoff_require_ready(observer->handoff);' \
	"$cpuclk" | sed -n '1s/:.*//p')
snapshot_line=$(grep -n -F \
	'mt6797_cpuclk_snapshot_locked(observer,' \
	"$cpuclk" | sed -n '$s/:.*//p')
if [ -z "$owner_line" ] || [ -z "$ready_line" ] || [ -z "$snapshot_line" ] ||
   [ "$owner_line" -ge "$ready_line" ] || [ "$ready_line" -ge "$snapshot_line" ]; then
	echo 'CPUCLK probe must bind owner, require READY, then take its snapshot' >&2
	exit 1
fi

# The provider owns the only resource reservation and one gate covers its
# validation/PM users as well as CPUCLK transactions. KUnit exercises blocked,
# faulted, callback-error, and mutual-exclusion paths without MMIO.
require_fixed 'handoff->cspm = devm_platform_ioremap_resource(pdev, 0);' "$handoff"
require_fixed 'mt6797_dvfsp_cspm_access_transaction(' "$handoff"
if [ "$(grep -F -c -- \
	'mt6797_dvfsp_cspm_access_lock(&handoff->cspm_access);' \
	"$handoff")" -lt 6 ]; then
	echo 'DVFSP validation, PM, and I2C paths must share the CSPM gate' >&2
	exit 1
fi
require_fixed 'mutex_lock(&access->lock);' "$access"
require_fixed 'if (ret == -EUCLEAN)' "$access"
require_fixed 'MT6797_DVFSP_PERMISSION_FAULTED);' "$access"
require_fixed 'mt6797_dvfsp_cspm_transaction_terminal' "$handoff"
# The actual private handoff transition remains statically tied to the tested
# access-gate callback: state, reason, and count change under both locks.
require_fixed 'handoff->state = MT6797_DVFSP_FAULTED;' "$handoff"
require_fixed 'handoff->reason = reason;' "$handoff"
require_fixed 'handoff->fault_count++;' "$handoff"
require_fixed 'mt6797_dvfsp_fault_locked(handoff, "cspm-transaction-terminal-failure");' "$handoff"
require_fixed 'MT6797_DVFSP_PERMISSION_FAULTED' "$access_test"
require_fixed 'mutex_trylock(&ctx->access->lock)' "$access_test"
require_fixed 'mt6797_dvfsp_cspm_serializes_concurrent_transactions' "$access_test"
require_fixed 'kthread_stop_put(first_task)' "$access_test"
require_fixed 'kthread_stop_put(second_task)' "$access_test"
require_fixed 'mt6797_dvfsp_i2c6_merge_transfer_result' "$handoff_header"
require_fixed 'mt6797_dvfsp_i2c6_merge_transfer_result(ret, lease_ret)' "$i2c"
reject_fixed 'if (!ret && lease_ret)' "$i2c"

echo 'test-mt6797-cpuclk-shared-cspm: PASS'
