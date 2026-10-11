#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production SYSIRQ callback against accepted and rejected MMIO writes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/irqchip/irq-mtk-sysirq.c").read_text()


def fragment(pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing production fragment: " + pattern)
    return match.group(0)


code = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long irq_hw_number_t;
typedef int raw_spinlock_t;
#define __iomem
#define BIT(n) (1UL << (n))
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW 8
struct irq_data;
struct irq_chip { int (*irq_set_type)(struct irq_data *, unsigned); };
struct irq_data {
    irq_hw_number_t hwirq;
    void *chip_data;
    struct irq_data *parent_data;
    struct irq_chip *chip;
};
static int locked;
static unsigned reads, writes, parent_calls, errors;
static unsigned parent_type;
static int parent_result;
static u32 *pending_address, pending_value, sibling_bit;
enum write_mode { ACCEPT, REJECT, SIBLING_CHANGE };
static enum write_mode mode;
#define raw_spin_lock_irqsave(lock, flags) \
    ((void)(lock), (flags) = 0, assert(!locked), locked = 1)
#define raw_spin_unlock_irqrestore(lock, flags) \
    ((void)(lock), (void)(flags), assert(locked), locked = 0)
#define pr_err_ratelimited(...) do { assert(!locked); errors++; } while (0)
static u32 readl_relaxed(void *address)
{
    assert(locked);
    reads++;
    if (pending_address) {
        assert(pending_address == address);
        if (mode != REJECT) *pending_address = pending_value;
        if (mode == SIBLING_CHANGE) *pending_address ^= sibling_bit;
        pending_address = NULL;
    }
    return *(u32 *)address;
}
static void writel_relaxed(u32 value, void *address)
{
    assert(locked && reads == 1 && !pending_address);
    writes++;
    pending_address = address;
    pending_value = value;
}
static int parent_set_type(struct irq_data *data, unsigned type)
{
    assert(locked && reads == 2 && writes == 1 && !pending_address);
    assert(data->chip && !data->parent_data);
    parent_calls++;
    parent_type = type;
    return parent_result;
}
'''
code += fragment(r"^struct mtk_sysirq_chip_data \{.*?^};\n")
code += fragment(r"^static int mtk_sysirq_set_type\(.*?^}\n")
code += r'''
int main(void)
{
    /* MT6797's two ordinary INTPOL banks contain eight and four words. */
    u32 banks[2][8], words[2] = { 8, 4 };
    void *bases[2] = { banks[0], banks[1] };
    u8 indices[384];
    u16 which_words[384];
    struct mtk_sysirq_chip_data controller = {
        .nr_intpol_bases = 2, .intpol_bases = bases, .intpol_words = words,
        .intpol_idx = indices, .which_word = which_words,
    };
    struct irq_chip parent_chip = { .irq_set_type = parent_set_type };
    struct irq_data parent = { .chip = &parent_chip };
    const unsigned types[] = {
        IRQ_TYPE_LEVEL_LOW, IRQ_TYPE_EDGE_FALLING,
        IRQ_TYPE_LEVEL_HIGH, IRQ_TYPE_EDGE_RISING,
    };
    for (unsigned i = 0; i < 384; i++) {
        indices[i] = i >= 256;
        which_words[i] = (i / 32) - (i >= 256 ? 8 : 0);
    }
    for (unsigned irq = 0; irq < 384; irq++) {
        struct irq_data data = {
            .hwirq = irq, .chip_data = &controller, .parent_data = &parent,
        };
        u32 bit = 1U << (irq % 32);
        for (unsigned t = 0; t < 4; t++) {
            unsigned translated = t < 2 ? types[t + 2] : types[t];
            for (mode = ACCEPT; mode <= SIBLING_CHANGE; mode++) {
                for (unsigned failure = 0; failure < 5; failure++) {
                    u32 before[2][8], expected[2][8];
                    memset(banks, 0xa5, sizeof(banks));
                    u32 *word = &banks[indices[irq]][which_words[irq]];
                    /* Force an actual polarity transition for both directions. */
                    if (t < 2) *word &= ~bit;
                    else *word |= bit;
                    memcpy(before, banks, sizeof(banks));
                    memcpy(expected, banks, sizeof(banks));
                    u32 *want = &expected[indices[irq]][which_words[irq]];
                    if (mode != REJECT) {
                        if (t < 2) *want |= bit;
                        else *want &= ~bit;
                    }
                    sibling_bit = 1U << ((irq + 1) % 32);
                    if (mode == SIBLING_CHANGE) *want ^= sibling_bit;
                    reads = writes = parent_calls = errors = 0;
                    parent_type = 0;
                    parent_result = failure == 1 ? -EINVAL : failure == 2 ? -EIO :
                                    failure > 2 ? (int)failure - 2 : 0;
                    int ret = mtk_sysirq_set_type(&data, types[t]);
                    assert(!locked && !pending_address && reads == 2 && writes == 1);
                    assert(!memcmp(banks, expected, sizeof(banks)));
                    if (mode == REJECT) {
                        assert(ret == -EIO && !parent_calls && errors == 1);
                        assert(!memcmp(banks, before, sizeof(banks)));
                    } else {
                        assert(ret == parent_result && parent_calls == 1 && !errors);
                        assert(parent_type == translated);
                    }
                }
            }
        }
    }
    /* An ignored write is admissible if the selected field was already correct. */
    struct irq_data data = {
        .hwirq = 240, .chip_data = &controller, .parent_data = &parent,
    };
    banks[0][7] |= 1U << 16;
    mode = REJECT;
    reads = writes = parent_calls = errors = 0;
    parent_result = 0;
    assert(!mtk_sysirq_set_type(&data, IRQ_TYPE_LEVEL_LOW));
    assert(!locked && !pending_address && reads == 2 && writes == 1);
    assert(parent_calls == 1 && parent_type == IRQ_TYPE_LEVEL_HIGH && !errors);
    puts("SYSIRQ polarity acceptance, rejection and parent propagation: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="mtk-sysirq-host-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    compiler = shlex.split(os.environ.get("HOSTCC", os.environ.get("CC", "cc")))
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    subprocess.run(compiler + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
                   + flags + [str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
