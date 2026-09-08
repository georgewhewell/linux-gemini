#!/usr/bin/env python3
"""Run real MediaTek gate registration/unwind against all six MT6797 tables.

Uses production descriptor lengths and onecell allocation expression. The
hardware adapter checks gate identity and allocated ID slots before access.
No MMIO or device access occurs.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
clk = root / "drivers/clk/mediatek"

def function(text, name):
    start = text.rfind("\n", 0, text.index(name + "(")) + 1
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

source = (clk / "clk-mtk.c").read_text()
start = source.index("num_clks = mcd->num_clks +")
end = source.index("clk_data = mtk_alloc_clk_data", start)
allocation = source[start:end]
registration = function((clk / "clk-gate.c").read_text(), "mtk_clk_register_gates")
unregistration = function((clk / "clk-gate.c").read_text(), "mtk_clk_unregister_gates")
shim = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define pr_warn(...) ((void)0)
#define pr_err(...) ((void)0)
#define dev_err_probe(d,e,...) (e)
struct device {}; struct device_node {}; struct regmap {};
struct clk_hw { unsigned id; };
struct clk_hw_onecell_data { struct clk_hw **hws; };
struct mtk_gate_regs { unsigned set_ofs, clr_ofs, sta_ofs; };
struct mtk_gate { unsigned id; const char *name; const char *parent_name;
 const struct mtk_gate_regs *regs; unsigned shift; const void *ops; };
int mtk_clk_gate_ops_setclr, mtk_clk_gate_ops_setclr_inv;
#define GATE_MTK(i,n,p,r,s,o) {i,n,p,r,s,o}
struct mtk_clk_desc { const struct mtk_gate *clks; size_t num_clks, num_clk_ids;
 size_t num_composite_clks, num_fixed_clks, num_factor_clks, num_mux_clks, num_divider_clks; };
static struct regmap map;
static struct regmap *device_node_to_regmap(struct device_node *n) {(void)n;return &map;}
static struct regmap *mtk_clk_get_hwv_regmap(struct device_node *n) {(void)n;return NULL;}
static const struct mtk_gate *first;
static size_t table_count, capacity, calls, alive, fail_at;
static struct clk_hw *mtk_clk_register_gate(struct device *d, const struct mtk_gate *g,
 struct regmap *m, struct regmap *v) {
 (void)d; assert(m == &map && !v);
 assert(g >= first && g < first + table_count && g->id < capacity);
 assert(g->name && strlen(g->name) && g->parent_name && strlen(g->parent_name));
 if (calls++ == fail_at) return ERR_PTR(-ENOMEM);
 struct clk_hw *hw = malloc(sizeof(*hw)); assert(hw); hw->id = g->id; alive++;return hw;
}
static void mtk_clk_unregister_gate(struct clk_hw *hw) {assert(alive);alive--;free(hw);}
'''
test = r'''
int main(void) {
 const struct mtk_clk_desc *mcd = &DESC;
 size_t num_clks;
 ALLOCATION
 assert(mcd->num_clks == ARRAY_SIZE(TABLE));
 assert(num_clks == BINDING_NR && mcd->num_clks + 1 == num_clks);
 capacity = num_clks; first = TABLE; table_count = ARRAY_SIZE(TABLE);
 for (fail_at = 0; fail_at <= table_count; fail_at++) {
  struct clk_hw_onecell_data data = { .hws = calloc(capacity, sizeof(*data.hws)) };
  assert(data.hws);calls = alive = 0;
  int ret = mtk_clk_register_gates(NULL, NULL, mcd->clks, mcd->num_clks, &data);
  if (fail_at < table_count) {assert(ret == -ENOMEM && alive == 0);}
  else {
   assert(!ret && alive == table_count && calls == table_count && !data.hws[0]);
   assert(data.hws[capacity - 1] && data.hws[capacity - 1]->id == capacity - 1);
   mtk_clk_unregister_gates(mcd->clks, mcd->num_clks, &data); assert(!alive);
  }
  for (size_t i = 0; i < capacity; i++) assert(IS_ERR_OR_NULL(data.hws[i]));
  free(data.hws);
 }
 puts("PASS DOMAIN: all binding slots, exact table length, registration and every allocation-failure unwind");
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-clock-bounds-") as directory:
    tmp = Path(directory)
    for domain in ["mm", "img", "vdec", "venc", "cam", "mjc"]:
        source = (clk / f"clk-mt6797-{domain}.c").read_text()
        start = source.index("static const struct mtk_gate_regs")
        end = source.index("\n};", source.index(f"static const struct mtk_clk_desc {domain}_desc")) + 3
        tables = source[start:end]
        body = test.replace("DESC", domain + "_desc").replace("TABLE", domain + "_clks")
        body = body.replace("BINDING_NR", "CLK_" + domain.upper() + "_NR").replace("DOMAIN", domain)
        body = body.replace("ALLOCATION", allocation)
        (tmp / "test.c").write_text(shim + (root / "include/dt-bindings/clock/mt6797-clk.h").read_text() + tables + registration + unregistration + body)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", str(tmp / "test.c"), "-o", str(tmp / "test")], check=True)
        subprocess.run([str(tmp / "test")], check=True)
