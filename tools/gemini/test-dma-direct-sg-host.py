#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native SG map/unmap callbacks and their public bounce flag lifecycle."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-root", type=Path,
                    default=Path(__file__).resolve().parents[2])
root = parser.parse_args().source_root


def fragment(path, pattern):
    match = re.search(pattern, (root / path).read_text(), re.M | re.S)
    if not match:
        raise RuntimeError("Missing native callback: " + pattern)
    return match.group(0)


code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t dma_addr_t;
typedef uint64_t phys_addr_t;
struct device { bool coherent; uint64_t offset; };
struct page { int index, route; };
struct scatterlist {
    struct page page;
    unsigned length, dma_length, dma_flags;
    dma_addr_t dma_address;
    struct scatterlist *next;
};
struct pci_p2pdma_map_state { void *mem; };
enum dma_data_direction { DMA_BIDIRECTIONAL };
enum { PCI_P2PDMA_MAP_NONE, PCI_P2PDMA_MAP_THRU_HOST_BRIDGE,
       PCI_P2PDMA_MAP_BUS_ADDR, PCI_P2PDMA_MAP_UNSUPPORTED };
#define DMA_MAPPING_ERROR UINT64_MAX
#define DMA_ATTR_SKIP_CPU_SYNC 1UL
#define fallthrough __attribute__((fallthrough))
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
#define sg_page(sg) (&(sg)->page)
#define for_each_sg(head, sg, count, i) \
    for ((i) = 0, (sg) = (head); (i) < (count); (i)++, (sg) = (sg)->next)
#if MARKERS
#define CONFIG_NEED_SG_DMA_FLAGS
#endif
/* Existing disabled-config public helpers intentionally ignore their input. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
'''
code += fragment("include/linux/scatterlist.h",
                 r"^#ifdef CONFIG_NEED_SG_DMA_FLAGS\n\n#define SG_DMA_BUS_ADDRESS.*?^#endif\t/\* CONFIG_NEED_SG_DMA_FLAGS \*/\n")
code += r'''
#pragma GCC diagnostic pop
#define ORIGINAL 0x10000ULL
#define POOL 0x90000ULL
#define PAGE_SIZE 4096ULL
static unsigned bounce, mapped, maps, unmaps, pool_queries, flushes;
static int fail_at;
static unsigned long unmap_attrs;
static bool dev_is_dma_coherent(struct device *dev) { return dev->coherent; }
static void arch_sync_dma_flush(void) { flushes++; }
static phys_addr_t sg_phys(struct scatterlist *sg)
{ return ORIGINAL + sg->page.index * PAGE_SIZE; }
static phys_addr_t dma_to_phys(struct device *dev, dma_addr_t address)
{ assert(address != DMA_MAPPING_ERROR); return address - dev->offset; }
static int pci_p2pdma_state(struct pci_p2pdma_map_state *state,
                           struct device *dev, struct page *page)
{ (void)state; (void)dev; return page->route; }
static dma_addr_t pci_p2pdma_bus_addr_map(void *mem, phys_addr_t phys)
{
    (void)mem;
    /* Deliberately looks like a pool address: bus addresses must not be tested. */
    return POOL + (phys - ORIGINAL);
}
static dma_addr_t map_phys(struct device *dev, phys_addr_t phys, size_t size,
                          enum dma_data_direction dir, unsigned long attrs)
{
    unsigned index = (phys - ORIGINAL) / PAGE_SIZE;
    (void)attrs;
    assert(index < 3 && size == PAGE_SIZE && dir == DMA_BIDIRECTIONAL);
    maps++;
    if ((int)index == fail_at) return DMA_MAPPING_ERROR;
    assert(!(mapped & (1U << index)));
    mapped |= 1U << index;
    return dev->offset + ((bounce & (1U << index)) ? POOL : ORIGINAL)
           + index * PAGE_SIZE;
}
static void unmap_phys(struct device *dev, dma_addr_t address, size_t size,
                       enum dma_data_direction dir, unsigned long attrs)
{
    phys_addr_t phys = dma_to_phys(dev, address);
    unsigned index = (phys - (phys >= POOL ? POOL : ORIGINAL)) / PAGE_SIZE;
    assert(index < 3 && size == PAGE_SIZE && dir == DMA_BIDIRECTIONAL);
    assert(mapped & (1U << index));
    mapped &= ~(1U << index);
    unmaps++;
    unmap_attrs |= attrs;
}
static dma_addr_t dma_direct_map_phys(struct device *dev, phys_addr_t phys,
    size_t size, enum dma_data_direction dir, unsigned long attrs, bool flush)
{ assert(!flush); return map_phys(dev, phys, size, dir, attrs); }
static void dma_direct_unmap_phys(struct device *dev, dma_addr_t address,
    size_t size, enum dma_data_direction dir, unsigned long attrs, bool flush)
{ assert(!flush); unmap_phys(dev, address, size, dir, attrs); }
static dma_addr_t iommu_dma_map_phys(struct device *dev, phys_addr_t phys,
    size_t size, enum dma_data_direction dir, unsigned long attrs)
{ return map_phys(dev, phys, size, dir, attrs); }
static void iommu_dma_unmap_phys(struct device *dev, dma_addr_t address,
    size_t size, enum dma_data_direction dir, unsigned long attrs)
{ unmap_phys(dev, address, size, dir, attrs); }
static __attribute__((unused)) void *swiotlb_find_pool(struct device *dev, phys_addr_t phys)
{
    (void)dev;
    pool_queries++;
    return phys >= POOL && phys < POOL + 3 * PAGE_SIZE ? (void *)1 : NULL;
}
'''
for path, name, return_type in [
    ("kernel/dma/direct.c", "dma_direct_unmap_sg", "void"),
    ("kernel/dma/direct.c", "dma_direct_map_sg", "int"),
    ("drivers/iommu/dma-iommu.c", "iommu_dma_unmap_sg_swiotlb", "static void"),
    ("drivers/iommu/dma-iommu.c", "iommu_dma_map_sg_swiotlb", "static int"),
]:
    code += fragment(path, rf"^{return_type} {name}\(.*?^}}\n")
code += r'''
static void reset(struct scatterlist *sg, unsigned bounce_mask)
{
    assert(!mapped);
    bounce = bounce_mask;
    fail_at = -1;
    maps = unmaps = pool_queries = flushes = 0;
    unmap_attrs = 0;
    for (int i = 0; i < 3; i++) {
        sg[i] = (struct scatterlist) {
            .page = { .index = i, .route = PCI_P2PDMA_MAP_NONE },
            .length = PAGE_SIZE, .next = i < 2 ? &sg[i + 1] : NULL,
        };
    }
}
int main(void)
{
    struct device dev = { .offset = 0x400000 };
    struct scatterlist sg[3];
    for (unsigned coherent = 0; coherent < 2; coherent++) {
        dev.coherent = coherent;
        /* First/last/mixed bounce, host-bridge P2P, and bus-address aliases. */
        for (unsigned mask = 0; mask < 8; mask++) {
            for (int bus = -1; bus < (MARKERS ? 3 : 0); bus++) {
                reset(sg, mask);
                sg[2].page.route = PCI_P2PDMA_MAP_THRU_HOST_BRIDGE;
                if (bus >= 0) sg[bus].page.route = PCI_P2PDMA_MAP_BUS_ADDR;
                unsigned expected = mask & (bus >= 0 ? ~(1U << bus) : ~0U);
                assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == 3);
                assert(sg_dma_is_swiotlb(sg) == (MARKERS && expected != 0));
                assert(!sg_dma_is_swiotlb(&sg[1]) && !sg_dma_is_swiotlb(&sg[2]));
                if (bus >= 0) assert(sg_dma_is_bus_address(&sg[bus]));
                dma_direct_unmap_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0);
                assert(!mapped && !sg_dma_is_swiotlb(sg));
                assert(unmaps == (unsigned)(bus >= 0 ? 2 : 3));
                assert(flushes == (coherent ? 0 : 2));
                for (int i = 0; i < 3; i++) assert(!sg_dma_is_bus_address(&sg[i]));
                /* Reuse the actual list after bounce teardown, now unbounced. */
                bounce = 0;
                assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == 3);
                assert(!sg_dma_is_swiotlb(sg));
                dma_direct_unmap_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0);
                assert(!mapped);
            }
        }
        /* Failure before/after a bounced segment must unwind the exact prefix. */
        for (int fail = 0; fail < 3; fail++) {
            reset(sg, 7);
            fail_at = fail;
            assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == -EIO);
            assert(!mapped && !sg_dma_is_swiotlb(sg) && unmaps == (unsigned)fail);
            if (fail) assert(unmap_attrs & DMA_ATTR_SKIP_CPU_SYNC);
        }
        if (MARKERS) {
            reset(sg, 4);
            sg[0].page.route = PCI_P2PDMA_MAP_BUS_ADDR;
            fail_at = 2;
            assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == -EIO);
            assert(!mapped && unmaps == 1 && !sg_dma_is_bus_address(sg));
            reset(sg, 1);
            sg[1].page.route = PCI_P2PDMA_MAP_UNSUPPORTED;
            assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == -EREMOTEIO);
            assert(!mapped && unmaps == 1 && !sg_dma_is_swiotlb(sg));
#if MARKERS
            /* Mapping clears only an obsolete bounce mark, not unrelated bits. */
            reset(sg, 0);
            sg[0].dma_flags = SG_DMA_SWIOTLB | 0x80;
            assert(dma_direct_map_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0) == 3);
            assert(sg[0].dma_flags == 0x80);
            dma_direct_unmap_sg(&dev, sg, 3, DMA_BIDIRECTIONAL, 0);
            assert(sg[0].dma_flags == 0x80);
#endif
        }
        /* IOMMU's existing whole-list marker also must retire on unmap/failure. */
        for (int fail = -1; fail < 3; fail++) {
            reset(sg, 7);
            fail_at = fail;
            int ret = iommu_dma_map_sg_swiotlb(&dev, sg, 3, DMA_BIDIRECTIONAL, 0);
            if (fail < 0) {
                assert(ret == 3 && sg_dma_is_swiotlb(sg) == (bool)MARKERS);
                iommu_dma_unmap_sg_swiotlb(&dev, sg, 3, DMA_BIDIRECTIONAL, 0);
            } else {
                assert(ret == -EIO && unmaps == (unsigned)fail);
                if (fail) assert(unmap_attrs & DMA_ATTR_SKIP_CPU_SYNC);
            }
            assert(!mapped && !sg_dma_is_swiotlb(sg));
        }
    }
    puts(MARKERS ? "native direct/IOMMU SG bounce lifecycle: PASS" :
                   "disabled SG marker helpers (not bounce admission): PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="dma-direct-sg-host-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    compiler = shlex.split(os.environ.get("HOSTCC", os.environ.get("CC", "cc")))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    for markers in (1, 0):
        subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
                       + flags + [f"-DMARKERS={markers}", str(path / "test.c"),
                                  "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
