#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile actual PTE/attribute code; this does not establish hardware success."""

import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/iommu/io-pgtable-arm.c").read_text()
header = (root / "include/linux/io-pgtable.h").read_text()
iommu = (root / "include/linux/iommu.h").read_text()
mmu = (root / "drivers/gpu/drm/panfrost/panfrost_mmu.c").read_text()
device = (root / "drivers/gpu/drm/panfrost/panfrost_device.h").read_text()


def function(text, signature):
    return signature + text.split(signature, 1)[1].split("\n}", 1)[0] + "\n}\n"


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1UL << (n))
typedef uint64_t arm_lpae_iopte;
struct io_pgtable_cfg {
    unsigned long quirks;
    unsigned int ias, oas;
    bool coherent_walk;
    struct { uint64_t memattr; } arm_mali_lpae_cfg;
};
'''
code += "enum io_pgtable_fmt {" + header.split("enum io_pgtable_fmt {", 1)[1].split("};", 1)[0] + "};\n"
code += "enum panfrost_gpu_quirks {" + device.split("enum panfrost_gpu_quirks {", 1)[1].split("};", 1)[0] + "};\n"
code += "\n".join(re.findall(r"^[ \t]*#define IO_PGTABLE_QUIRK[^\n]+", header, re.M)) + "\n"
code += "\n".join(re.findall(r"^#define IOMMU_(?:READ|WRITE|CACHE|NOEXEC|MMIO|PRIV)\s[^\n]+", iommu, re.M)) + "\n"
code += source.split("/* Page table bits */", 1)[1].split("/* IOPTE accessors */", 1)[0]
code += r'''
struct arm_lpae_io_pgtable {
    struct { enum io_pgtable_fmt fmt; struct io_pgtable_cfg cfg; } iop;
};
struct panfrost_compatible { unsigned long gpu_quirks; };
struct panfrost_device { bool coherent; struct panfrost_compatible *comp; };
struct panfrost_mmu { struct io_pgtable_cfg pgtbl_cfg; };
'''
code += function(source, "static arm_lpae_iopte arm_lpae_prot_to_pte(")
allocation = source.split("arm_mali_lpae_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)", 1)[1]
attributes = allocation.split("\tcfg->arm_mali_lpae_cfg.memattr =", 1)[1].split("\n\tdata->pgd =", 1)[0]
code += "static void attributes(struct io_pgtable_cfg *cfg) {\n\tcfg->arm_mali_lpae_cfg.memattr =" + attributes + "\n}\n"
validation = allocation.split("\tif (cfg->quirks", 1)[1].split("\n\tcfg->pgsize_bitmap", 1)[0]
code += "static void *valid(struct io_pgtable_cfg *cfg) {\n\tif (cfg->quirks" + validation + "\nreturn (void *)1;\n}\n"
gate = mmu.split("\tif (fmt == ARM_MALI_LPAE && !pfdev->coherent &&", 1)[1].split("\n\tmmu->pgtbl_ops", 1)[0]
code += "static void gate(struct panfrost_mmu *mmu, struct panfrost_device *pfdev, enum io_pgtable_fmt fmt) {\n\tif (fmt == ARM_MALI_LPAE && !pfdev->coherent &&" + gate + "\n}\n"
code += r'''
int main(void)
{
    const unsigned long q = IO_PGTABLE_QUIRK_ARM_MALI_INNER_SHAREABLE;
    struct arm_lpae_io_pgtable table = { .iop.fmt = ARM_MALI_LPAE };
    const int rw = IOMMU_READ | IOMMU_WRITE;
    uint64_t old, pte;
    unsigned int flags;

    /* Every combination retains permissions, indices and execute policy. */
    for (flags = 0; flags < 64; flags++) {
        table.iop.cfg.quirks = 0;
        old = arm_lpae_prot_to_pte(&table, flags);
        assert(((old >> 8) & 3) == 2);
        table.iop.cfg.quirks = q;
        pte = arm_lpae_prot_to_pte(&table, flags);
        assert((pte & ~(3ULL << 8)) == (old & ~(3ULL << 8)));
        assert(((pte >> 8) & 3) ==
               ((flags & IOMMU_CACHE) && !(flags & IOMMU_MMIO) ? 3 : 0));
    }
    table.iop.cfg.quirks = 0;
    attributes(&table.iop.cfg);
    assert(table.iop.cfg.arm_mali_lpae_cfg.memattr == 0x888d88);
    table.iop.cfg.quirks = q;
    attributes(&table.iop.cfg);
    assert(table.iop.cfg.arm_mali_lpae_cfg.memattr == 0x484d48);
    assert(valid(&table.iop.cfg));
    table.iop.cfg.coherent_walk = true;
    assert(!valid(&table.iop.cfg));
    table.iop.cfg.quirks = 0;
    assert(valid(&table.iop.cfg));
    table.iop.cfg.coherent_walk = false;
    table.iop.cfg.quirks = BIT(31);
    assert(!valid(&table.iop.cfg));

    /* Non-Mali encoding is unchanged even if the quirk is misrouted. */
    for (table.iop.fmt = ARM_32_LPAE_S1;
         table.iop.fmt <= ARM_64_LPAE_S2; table.iop.fmt++) {
        table.iop.cfg.quirks = 0;
        old = arm_lpae_prot_to_pte(&table, rw | IOMMU_CACHE);
        table.iop.cfg.quirks = q;
        assert(arm_lpae_prot_to_pte(&table, rw | IOMMU_CACHE) == old);
    }
    struct panfrost_compatible comp = {0};
    struct panfrost_device gpu = { .comp = &comp };
    struct panfrost_mmu mmu = {0};
    gate(&mmu, &gpu, ARM_MALI_LPAE);
    assert(!mmu.pgtbl_cfg.quirks);
    comp.gpu_quirks = BIT(GPU_QUIRK_MALI_INNER_SHAREABLE);
    gpu.coherent = true;
    gate(&mmu, &gpu, ARM_MALI_LPAE);
    assert(!mmu.pgtbl_cfg.quirks);
    gpu.coherent = false;
    gate(&mmu, &gpu, ARM_64_LPAE_S1);
    assert(!mmu.pgtbl_cfg.quirks);
    gate(&mmu, &gpu, ARM_MALI_LPAE);
    assert(mmu.pgtbl_cfg.quirks == q);
    puts("PASS: actual legacy MMU attributes, 64 PTE combinations, format/coherency gates");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="gemini-shareability-test-") as tmp:
    test_source = Path(tmp) / "test.c"
    binary = Path(tmp) / "test"
    test_source.write_text(code)
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", str(test_source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
