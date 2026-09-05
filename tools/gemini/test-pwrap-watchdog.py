#!/usr/bin/env python3
"""Run the actual MT6797 wrapper admission helper with instrumented callbacks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/soc/mediatek/mtk-pmic-wrap.c").read_text()
start = source.index("static int pwrap_enable_clocks(")
end = source.index("\n}\n", start) + 3
helper = source[start:end]
probe = source[source.index("static int pwrap_probe("):]
admission = probe.index("pwrap_enable_clocks(wrp, &clk)")
assert "devm_clk_bulk_get_all_enabled(" not in probe
for mutation in ("pwrap_writel(", "pwrap_init(", "devm_request_irq("):
    assert admission < probe.index(mutation), mutation
prefix = probe[:admission]
assert "reset_control_assert(" not in prefix
assert "reset_control_deassert(" not in prefix
assert "reset_control_reset(" not in prefix
c = r'''
#include <assert.h>
#include <stdio.h>
#define PWRAP_MT6797 7
struct device { int unused; };
struct clk_bulk_data { int unused; };
struct master { int type; };
struct pmic_wrapper { struct master *master; struct device *dev; };
static int checks, clocks, guard_result, clock_result;
static int mtk_wdt_require_boot_recovery(struct device *d)
{ (void)d; assert(!clocks); checks++; return guard_result; }
static int dev_err_probe(struct device *d, int result, const char *message)
{ (void)d; (void)message; return result; }
static int devm_clk_bulk_get_all_enabled(struct device *d, struct clk_bulk_data **c)
{ (void)d; (void)c; clocks++; return clock_result; }
''' + helper + r'''
int main(void)
{
 struct master master = {.type=PWRAP_MT6797};
 struct pmic_wrapper wrapper = {.master=&master};
 struct clk_bulk_data *clk = 0;
 int failures[] = {-22, -517, -19, -1, -95};
 for (unsigned i=0; i<sizeof(failures)/sizeof(*failures); i++) {
  checks=clocks=0; guard_result=failures[i];
  assert(pwrap_enable_clocks(&wrapper,&clk)==failures[i]);
  assert(checks==1 && clocks==0);
 }
 for (int result=-1; result<=4; result++) {
  checks=clocks=0; guard_result=0; clock_result=result;
  assert(pwrap_enable_clocks(&wrapper,&clk)==result);
  assert(checks==1 && clocks==1);
 }
 /* Every other master type follows the original clock-only path. */
 for (int type=0; type<32; type++) {
  if (type==PWRAP_MT6797) continue;
  master.type=type; checks=clocks=0; guard_result=-22; clock_result=2;
  assert(pwrap_enable_clocks(&wrapper,&clk)==2);
  assert(checks==0 && clocks==1);
 }
 puts("PASS actual pwrap helper: guard rejection, ordering, clock results, other SoCs unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-pwrap-watchdog-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(c)
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True, timeout=5)
