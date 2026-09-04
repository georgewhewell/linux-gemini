#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

helper=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/capture-vgpu-toggle-preflight.sh
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

make_fixture()
{
	root=$1
	rm -rf "$root"
	mkdir -p "$root/sys/firmware/devicetree/base/i2c@11010000/regulator@1c"
	mkdir -p "$root/sys/firmware/devicetree/base/gpu@13040000"
	mkdir -p "$root/sys/bus/platform/devices/vgpu-toggle"
	mkdir -p "$root/sys/bus/platform/drivers/reg-userspace-consumer"
	mkdir -p "$root/sys/bus/i2c/devices/0-001c/regulator/regulator.0"
	mkdir -p "$root/sys/bus/i2c/drivers/rt5735-regulator"
	mkdir -p "$root/sys/class/regulator"
	mkdir -p "$root/sys/bus/platform/drivers/panfrost"
	mkdir -p "$root/debug/pm_genpd"
	mkdir -p "$root/debug/regmap/0-001c"
	mkdir -p "$root/proc"
	for clock in infra_i2c_gpupm infra_ap_dma; do
		mkdir -p "$root/debug/clk/$clock"
		printf '0\n' > "$root/debug/clk/$clock/clk_enable_count"
		printf '0\n' > "$root/debug/clk/$clock/clk_prepare_count"
	done
	printf 'okay\000' > "$root/sys/firmware/devicetree/base/i2c@11010000/regulator@1c/status"
	printf 'disabled\000' > "$root/sys/firmware/devicetree/base/gpu@13040000/status"
	printf 'disabled\n' > "$root/sys/bus/platform/devices/vgpu-toggle/state"
	ln -s ../../drivers/reg-userspace-consumer \
		"$root/sys/bus/platform/devices/vgpu-toggle/driver"
	printf 'vgpu\n' > "$root/sys/bus/i2c/devices/0-001c/regulator/regulator.0/name"
	printf 'disabled\n' > "$root/sys/bus/i2c/devices/0-001c/regulator/regulator.0/state"
	printf '843750\n' > "$root/sys/bus/i2c/devices/0-001c/regulator/regulator.0/microvolts"
	printf '0\n' > "$root/sys/bus/i2c/devices/0-001c/regulator/regulator.0/num_users"
	ln -s "$root/sys/firmware/devicetree/base/i2c@11010000/regulator@1c" \
		"$root/sys/bus/i2c/devices/0-001c/of_node"
	ln -s ../../bus/i2c/devices/0-001c/regulator/regulator.0 \
		"$root/sys/class/regulator/regulator.0"
	ln -s ../../drivers/rt5735-regulator "$root/sys/bus/i2c/devices/0-001c/driver"
	for domain in mfg_async mfg mfg_core0 mfg_core1 mfg_core2 mfg_core3; do
		printf '%s off-0\n' "$domain" >> "$root/debug/pm_genpd/pm_genpd_summary"
	done
	printf 'regulator_ignore_unused\n' > "$root/proc/cmdline"
	: > "$root/proc/interrupts"
	printf 'rt5735-regulator\n' > "$root/debug/regmap/0-001c/name"
	printf '11: 27\n' > "$root/debug/regmap/0-001c/registers"
	cat > "$root/dmesg" <<'EOF'
#!/bin/sh
echo 'rt5735-regulator 0-001c: ready'
EOF
	chmod +x "$root/dmesg"
	cat > "$root/write-state" <<'EOF'
#!/bin/sh
state_file=$1
state=$2
root=${state_file%%/sys/*}
rail=$root/sys/class/regulator/regulator.0
registers=$root/debug/regmap/0-001c/registers
case ${WRITE_MODE:-success}:$state in
	enable_fail:enabled) exit 1 ;;
	term_after_enable:enabled)
		printf 'enabled\n' > "$state_file"
		printf 'enabled\n' > "$rail/state"
		printf '1\n' > "$rail/num_users"
		printf '11: a7\n' > "$registers"
		kill -TERM "$PPID" ;;
	active_bad:enabled)
		printf 'enabled\n' > "$state_file"
		printf 'enabled\n' > "$rail/state"
		printf '900000\n' > "$rail/microvolts"
		printf '1\n' > "$rail/num_users"
		printf '11: a7\n' > "$registers" ;;
	raw_bad:enabled)
		printf 'enabled\n' > "$state_file"
		printf 'enabled\n' > "$rail/state"
		printf '1\n' > "$rail/num_users"
		printf '11: a6\n' > "$registers" ;;
	disable_fail:disabled) exit 1 ;;
	disable_noop:disabled) ;;
	*:enabled)
		printf 'enabled\n' > "$state_file"
		printf 'enabled\n' > "$rail/state"
		printf '1\n' > "$rail/num_users"
		printf '11: a7\n' > "$registers" ;;
	*:disabled)
		printf 'disabled\n' > "$state_file"
		printf 'disabled\n' > "$rail/state"
		printf '843750\n' > "$rail/microvolts"
		printf '0\n' > "$rail/num_users"
		printf '11: 27\n' > "$registers" ;;
esac
EOF
	chmod +x "$root/write-state"
}

run_helper()
{
	root=$1
	shift
	env SYSFS_ROOT="$root/sys" DEBUGFS_ROOT="$root/debug" \
		PROC_ROOT="$root/proc" DMESG="$root/dmesg" \
		STATE_WRITE="$root/write-state" "$@" "$helper"
}

success=$tmp/success
make_fixture "$success"
run_helper "$success" > "$success/output"
grep -Fqx 'VGPU_TOGGLE_PREFLIGHT=PASS before=0x27 active=0xa7 after=0x27' \
	"$success/output"
test "$(cat "$success/sys/class/regulator/regulator.0/state")" = disabled
test "$(cat "$success/sys/class/regulator/regulator.0/microvolts")" = 843750
test "$(cat "$success/sys/class/regulator/regulator.0/num_users")" = 0

for mode in enable_fail term_after_enable active_bad raw_bad disable_fail disable_noop; do
	fixture=$tmp/$mode
	make_fixture "$fixture"
	if run_helper "$fixture" env WRITE_MODE="$mode" > "$fixture/output" 2>&1; then
		echo "FAIL: $mode unexpectedly passed" >&2
		exit 1
	fi
	case $mode in
	disable_fail|disable_noop) ;;
	*)
		test "$(cat "$fixture/sys/class/regulator/regulator.0/state")" = disabled
		test "$(cat "$fixture/sys/class/regulator/regulator.0/microvolts")" = 843750
		test "$(cat "$fixture/sys/class/regulator/regulator.0/num_users")" = 0
		;;
	esac
done

not_ready=$tmp/not_ready
make_fixture "$not_ready"
unlink "$not_ready/sys/bus/platform/devices/vgpu-toggle/driver"
if run_helper "$not_ready" env READY_TRIES=2 READY_SLEEP=0.01 > "$not_ready/output" 2>&1; then
	echo 'FAIL: unbound output unexpectedly passed the readiness deadline' >&2
	exit 1
fi
grep -Fq 'did not bind before readiness deadline' "$not_ready/output"

echo 'PASS: G2 helper restores recoverable failures and detects no-op cleanup'
