#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise real MTU3 fallback/interrupt/pull-up functions with fake MMIO."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'drivers/usb/mtu3/mtu3_core.c').read_text()
header = (root / 'drivers/usb/mtu3/mtu3.h').read_text()
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef int irqreturn_t;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define IRQ_HANDLED 1
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 5
#define dev_dbg(...) ((void)0)
#define dev_info(...) ((void)0)
static u32 traced_intr, traced_raw, traced_enabled;
static void trace_mtu3_u3_ltssm_isr(u32 intr, u32 raw, u32 enabled)
{ traced_intr = intr; traced_raw = raw; traced_enabled = enabled; }
#include "mtu3_hw_regs.h"
struct mtu3 {
    void *mac_base, *ippc_base;
    bool u3_capable, manual_u2_fallback, is_active, softconnect;
    int speed;
};
static u32 mac[0x3000 / 4], ippc[0x100 / 4];
static unsigned int resets;
static u32 mtu3_readl(void *base, unsigned int offset)
{ return *(u32 *)((char *)base + offset); }
static void mtu3_writel(void *base, unsigned int offset, u32 value)
{
    u32 *reg = (u32 *)((char *)base + offset);
    if (base == mac && offset == U3D_LTSSM_INTR) *reg &= ~value;
    else *reg = value;
}
static void mtu3_setbits(void *base, unsigned int off, u32 bits)
{ mtu3_writel(base, off, mtu3_readl(base, off) | bits); }
static void mtu3_clrbits(void *base, unsigned int off, u32 bits)
{ mtu3_writel(base, off, mtu3_readl(base, off) & ~bits); }
static void mtu3_gadget_reset(struct mtu3 *m) { (void)m; resets++; }
static void mtu3_gadget_suspend(struct mtu3 *m) { (void)m; }
static void mtu3_gadget_resume(struct mtu3 *m) { (void)m; }
'''
code += re.search(r'^#define SSUSB_U2_CTRL\(p\).*$', header, re.M).group(0) + '\n'
for name in ('mtu3_ss_func_set', 'mtu3_hs_softconn_set', 'mtu3_intr_enable',
             'mtu3_csr_init', 'mtu3_dev_on_off', 'mtu3_u3_ltssm_isr'):
    match = re.search(r'^(?:static (?:inline )?)?(?:void|irqreturn_t) ' +
                      name + r'\(.*?^}\n', source, re.M | re.S)
    if not match:
        raise RuntimeError('Missing real driver function: ' + name)
    code += match.group(0)
code += r'''
int main(void)
{
    for (int manual = 0; manual <= 1; manual++) {
        struct mtu3 m = { .mac_base = mac, .ippc_base = ippc,
            .u3_capable = true, .manual_u2_fallback = manual,
            .is_active = true, .softconnect = true, .speed = USB_SPEED_SUPER };
        memset(mac, 0, sizeof(mac)); memset(ippc, 0, sizeof(ippc));
        mac[U3D_USB2_TEST_MODE / 4] = BIT(31) | U2U3_AUTO_SWITCH;
        mac[U3D_U3U2_SWITCH_CTRL / 4] = BIT(31);
        mtu3_csr_init(&m);
        assert(mac[U3D_USB2_TEST_MODE / 4] ==
               (BIT(31) | (manual ? 0 : U2U3_AUTO_SWITCH)));
        assert(mac[U3D_U3U2_SWITCH_CTRL / 4] ==
               (BIT(31) | (manual ? 0 : SOFTCON_CLR_AUTO_EN)));
        mtu3_intr_enable(&m);
        assert(!!(mac[U3D_LTSSM_INTR_ENABLE / 4] & SS_DISABLE_INTR) == manual);
        for (int active = 0; active <= 1; active++) {
            for (int connect = 0; connect <= 1; connect++) {
                m.is_active = active; m.softconnect = connect;
                mac[U3D_POWER_MANAGEMENT / 4] = BIT(31);
                ippc[SSUSB_U2_CTRL(0) / 4] = BIT(30) | SSUSB_U2_PORT_PDN;
                mac[U3D_LTSSM_INTR / 4] = SS_DISABLE_INTR | RXDET_SUCCESS_INTR;
                assert(mtu3_u3_ltssm_isr(&m) == IRQ_HANDLED);
                assert(traced_raw == (SS_DISABLE_INTR | RXDET_SUCCESS_INTR));
                assert(traced_enabled == mac[U3D_LTSSM_INTR_ENABLE / 4]);
                assert(traced_intr == (traced_raw & traced_enabled));
                bool fallback = manual && active && connect;
                assert(mac[U3D_POWER_MANAGEMENT / 4] == (BIT(31) |
                       (fallback ? SOFT_CONN | SUSPENDM_ENABLE : 0)));
                assert(ippc[SSUSB_U2_CTRL(0) / 4] == (BIT(30) |
                       (fallback ? 0 : SSUSB_U2_PORT_PDN)));
                assert(mac[U3D_LTSSM_INTR / 4] ==
                       (RXDET_SUCCESS_INTR | (manual ? 0 : SS_DISABLE_INTR)));
            }
        }
        m.is_active = m.softconnect = true;
        mac[U3D_USB3_CONFIG / 4] = USB3_EN | BIT(30);
        mac[U3D_POWER_MANAGEMENT / 4] = BIT(31) | SOFT_CONN | SUSPENDM_ENABLE;
        mtu3_dev_on_off(&m, 0);
        assert(mac[U3D_USB3_CONFIG / 4] == BIT(30));
        assert(mac[U3D_POWER_MANAGEMENT / 4] ==
               (BIT(31) | (manual ? 0 : SOFT_CONN | SUSPENDM_ENABLE)));
        mtu3_dev_on_off(&m, 1);
        assert(mac[U3D_USB3_CONFIG / 4] == (USB3_EN | BIT(30)));
        mac[U3D_LTSSM_INTR_ENABLE / 4] |= VBUS_FALL_INTR;
        mac[U3D_LTSSM_INTR / 4] = VBUS_FALL_INTR | SS_DISABLE_INTR;
        mac[U3D_POWER_MANAGEMENT / 4] = BIT(31) | SOFT_CONN | SUSPENDM_ENABLE;
        resets = 0;
        mtu3_u3_ltssm_isr(&m);
        assert(resets == 1);
        assert(!(mac[U3D_USB3_CONFIG / 4] & USB3_EN));
        assert(mac[U3D_POWER_MANAGEMENT / 4] ==
               (BIT(31) | (manual ? 0 : SOFT_CONN | SUSPENDM_ENABLE)));
        m.speed = USB_SPEED_HIGH;
        mtu3_dev_on_off(&m, 1);
        assert(mac[U3D_POWER_MANAGEMENT / 4] & SOFT_CONN);
        mtu3_dev_on_off(&m, 0);
        assert(!(mac[U3D_POWER_MANAGEMENT / 4] & SOFT_CONN));
    }
    puts("PASS: MTU3 real fallback, masked IRQs, disconnect, VBUS loss and other IP");
}
'''
with tempfile.TemporaryDirectory(prefix='mtu3-fallback-') as tmp:
    c = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    c.write_text(code)
    subprocess.run(shlex.split(os.environ.get('HOSTCC', os.environ.get('CC', 'cc'))) +
                   ['-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'drivers/usb/mtu3'), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
