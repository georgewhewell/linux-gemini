#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production dev_pm_opp_of_get_sharing_cpus() on the Gemini topology.

The function is extracted verbatim from drivers/opp/of.c and run on the host
with device-tree lookups bound to a model of the Gemini CPU nodes: cpu0-3
share an opp-shared LL table, cpu4-7 an opp-shared L table, and the A72
cpu8/9 have no operating-points-v2.  On M41 the scan for cpu0 aborted with
-ENOENT at cpu8, cpufreq-dt then fell back to a one-CPU policy and failed
with -EBUSY on cpu1.  This checks that each A53 cluster now resolves to its
four CPUs and that a CPU without a table still reports -ENOENT itself.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/opp/of.c').read_text()
start = source.index('int dev_pm_opp_of_get_sharing_cpus(')
function = source[start:source.index('\n}\n', start) + 3]

c = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define NR 10
struct device_node { int table; };           /* 0: none, 1: LL, 2: L */
struct device { int id; struct device_node *of_node; };
struct cpumask { unsigned long bits; };
static struct device_node cpu_nodes[NR] = {
	{1}, {1}, {1}, {1}, {2}, {2}, {2}, {2}, {0}, {0},
};
static struct device_node tables[3];
static int errors;
static void __free_device_node(struct device_node **np) { (void)np; }
#define __free(x) __attribute__((cleanup(__free_##x)))
#define for_each_possible_cpu(c) for ((c) = 0; (c) < NR; (c)++)
static void cpumask_set_cpu(int cpu, struct cpumask *m) { m->bits |= 1UL << cpu; }
static struct device_node *_opp_of_get_opp_desc_node(struct device_node *np, int i)
{ (void)i; return np->table ? &tables[np->table] : NULL; }
static struct device_node *dev_pm_opp_of_get_opp_desc_node(struct device *dev)
{ return _opp_of_get_opp_desc_node(dev->of_node, 0); }
static struct device_node *of_cpu_device_node_get(int cpu) { return &cpu_nodes[cpu]; }
static bool of_property_read_bool(struct device_node *np, const char *p)
{ (void)np; return !strcmp(p, "opp-shared"); }
#define dev_dbg(...) do { } while (0)
#define dev_err(...) (errors++)
#define pr_err(...) (errors++)
#define EXPORT_SYMBOL_GPL(x)
''' + function + r'''
int main(void)
{
	struct device dev;
	struct cpumask m;

	for (int cpu = 0; cpu < NR; cpu++) {
		dev = (struct device){ cpu, &cpu_nodes[cpu] };
		m.bits = 0;
		int ret = dev_pm_opp_of_get_sharing_cpus(&dev, &m);
		if (cpu < 4) { assert(ret == 0 && m.bits == 0x00f); }
		else if (cpu < 8) { assert(ret == 0 && m.bits == 0x0f0); }
		else { assert(ret == -ENOENT && m.bits == 0); }
	}
	assert(errors == 0);
	puts("opp-of-sharing-cpus: pass (LL 0-3, L 4-7, A72 without table -ENOENT)");
	return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gemini-opp-sharing-') as tmp:
    src, exe = Path(tmp, 'test.c'), Path(tmp, 'test')
    src.write_text(c)
    subprocess.run([os.environ.get('HOSTCC', 'cc'), '-std=gnu11', '-Wall',
                    '-Wextra', '-Werror', '-o', str(exe), str(src)], check=True)
    subprocess.run([str(exe)], check=True)
