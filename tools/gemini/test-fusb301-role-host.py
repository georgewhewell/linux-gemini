#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the FUSB301 attachment/removal code with failing controller providers."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "drivers/usb/typec/fusb301.c").read_text()


def fragment(pattern):
    match = re.search(pattern, source, re.M | re.S)
    if not match:
        raise RuntimeError("Missing driver fragment: " + pattern)
    return match.group(0)


code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((~0U >> (31 - (h))) & (~0U << (l)))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define dev_info(...) ((void)0)
enum typec_port_type { TYPEC_PORT_SRC, TYPEC_PORT_SNK, TYPEC_PORT_DRP };
enum typec_role { TYPEC_SINK, TYPEC_SOURCE };
enum typec_data_role { TYPEC_DEVICE, TYPEC_HOST };
enum typec_orientation { TYPEC_ORIENTATION_NONE, TYPEC_ORIENTATION_NORMAL,
                         TYPEC_ORIENTATION_REVERSE };
enum typec_pwr_opmode { TYPEC_PWR_MODE_USB, TYPEC_PWR_MODE_1_5A,
                        TYPEC_PWR_MODE_3_0A };
enum usb_role { USB_ROLE_NONE, USB_ROLE_HOST, USB_ROLE_DEVICE };
enum { TYPEC_ACCESSORY_NONE, TYPEC_ACCESSORY_AUDIO, TYPEC_ACCESSORY_DEBUG };
struct device { int unused; };
struct regmap { unsigned int status, type; };
struct regulator { bool on; int enable_error; };
struct typec_capability { enum typec_port_type type; int prefer_role; };
struct typec_port {
    enum typec_orientation orientation;
    enum typec_data_role data;
    enum typec_role power;
    enum typec_pwr_opmode opmode;
};
struct typec_partner { int accessory; };
struct typec_partner_desc { int accessory; };
struct usb_role_switch { enum usb_role role; int error; };
struct i2c_client { struct device dev; int irq; void *data; };
static struct typec_partner partner;
static int partners, orientation_error, partner_error;
static bool removing, irq_live, port_live, switch_released;
'''
code += "\n".join(re.findall(r"^#define FUSB301_.*$", source, re.M)) + "\n"
code += fragment(r"^struct fusb301 \{.*?^};\n")
code += r'''
static int regmap_read(struct regmap *map, unsigned reg, unsigned *value)
{
    assert(reg == FUSB301_REG_STATUS || reg == FUSB301_REG_TYPE);
    *value = reg == FUSB301_REG_STATUS ? map->status : map->type;
    return 0;
}
static int regulator_enable(struct regulator *r)
{
    if (r->enable_error) return r->enable_error;
    assert(!r->on);
    r->on = true;
    return 0;
}
static int regulator_disable(struct regulator *r)
{
    assert(r->on);
    r->on = false;
    return 0;
}
static int usb_role_switch_set_role(struct usb_role_switch *sw, enum usb_role role)
{
    if (removing) assert(!irq_live);
    if (!sw) return 0;
    if (sw->error) return sw->error;
    sw->role = role;
    return 0;
}
static int typec_set_orientation(struct typec_port *p, enum typec_orientation o)
{
    assert(port_live);
    if (orientation_error && o != TYPEC_ORIENTATION_NONE)
        return orientation_error;
    p->orientation = o;
    return 0;
}
static void typec_set_data_role(struct typec_port *p, enum typec_data_role role)
{ p->data = role; }
static void typec_set_pwr_role(struct typec_port *p, enum typec_role role)
{ p->power = role; }
static void typec_set_vconn_role(struct typec_port *p, enum typec_role role)
{ (void)p; (void)role; }
static void typec_set_pwr_opmode(struct typec_port *p, enum typec_pwr_opmode mode)
{ p->opmode = mode; }
static struct typec_partner *typec_register_partner(struct typec_port *p,
                                                   struct typec_partner_desc *d)
{
    (void)p;
    assert(!partners);
    if (partner_error) return ERR_PTR(partner_error);
    partners++;
    partner.accessory = d->accessory;
    return &partner;
}
static void typec_unregister_partner(struct typec_partner *p)
{ assert(p == &partner && partners == 1); partners--; }
static void typec_unregister_port(struct typec_port *p)
{ (void)p; assert(!irq_live && !partners); port_live = false; }
static void *i2c_get_clientdata(struct i2c_client *c) { return c->data; }
static void devm_free_irq(struct device *d, int irq, void *data)
{ (void)d; (void)irq; (void)data; assert(irq_live); irq_live = false; }
static void usb_role_switch_put(struct usb_role_switch *sw)
{ (void)sw; assert(!port_live); switch_released = true; }
'''
for name in ("fusb301_set_vbus", "fusb301_default_role", "fusb301_orientation",
             "fusb301_sink_opmode", "fusb301_unregister_partner",
             "fusb301_disconnect", "fusb301_update_status", "fusb301_remove"):
    code += fragment(r"^static [\w ]+ " + name + r"\(.*?^}\n")

code += r'''
int main(void)
{
    struct regmap map = {0};
    struct regulator vbus = {0};
    struct typec_port port = {0};
    struct usb_role_switch sw = {0};
    struct fusb301 fusb = {
        .regmap = &map, .vbus = &vbus, .port = &port, .role_sw = &sw,
        .cap.type = TYPEC_PORT_DRP,
    };
    struct i2c_client client = { .data = &fusb };
    port_live = true;
    assert(!fusb301_update_status(&fusb));
    assert(sw.role == USB_ROLE_NONE && !vbus.on && !partners);

    for (unsigned orientation = 0x10; orientation <= 0x20; orientation += 0x10) {
        /* A downstream device requires host data role and source power. */
        map.status = 1 | orientation;
        map.type = 0x10;
        assert(!fusb301_update_status(&fusb));
        assert(sw.role == USB_ROLE_HOST && vbus.on && partners == 1);
        assert(port.data == TYPEC_HOST && port.power == TYPEC_SOURCE);
        assert(port.orientation == (orientation == 0x10 ?
               TYPEC_ORIENTATION_NORMAL : TYPEC_ORIENTATION_REVERSE));
        assert(!fusb301_update_status(&fusb) && partners == 1);
        map.status = 0;
        assert(!fusb301_update_status(&fusb));
        assert(sw.role == USB_ROLE_NONE && !vbus.on && !partners);
        assert(port.orientation == TYPEC_ORIENTATION_NONE);
    }

    /* An upstream host supplies power; we must not drive VBUS ourselves. */
    map.status = 1 | 0x10 | 0x6;
    map.type = 0x8;
    assert(!fusb301_update_status(&fusb));
    assert(sw.role == USB_ROLE_DEVICE && !vbus.on && partners == 1);
    assert(port.data == TYPEC_DEVICE && port.power == TYPEC_SINK);
    assert(port.opmode == TYPEC_PWR_MODE_3_0A);

    map.type = 0x1; /* Audio accessory must not start USB data. */
    assert(!fusb301_update_status(&fusb));
    assert(sw.role == USB_ROLE_NONE && !vbus.on);
    assert(partner.accessory == TYPEC_ACCESSORY_AUDIO);
    map.status = 0;
    assert(!fusb301_update_status(&fusb));

    /* Failed attach never leaves source power or a published partner. */
    map.status = 1 | 0x10;
    map.type = 0x10;
    int *faults[] = { &orientation_error, &vbus.enable_error,
                     &sw.error, &partner_error };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        *faults[i] = -EIO;
        assert(fusb301_update_status(&fusb) == -EIO);
        assert(!vbus.on && !fusb.vbus_on && !partners);
        assert(!fusb.partner_type && sw.role == USB_ROLE_NONE);
        assert(port.orientation == TYPEC_ORIENTATION_NONE);
        *faults[i] = 0;
    }

    /* A failed data disconnect must still remove VBUS and the partner. */
    assert(!fusb301_update_status(&fusb));
    sw.error = -EIO;
    map.status = 0;
    assert(fusb301_update_status(&fusb) == -EIO);
    assert(!vbus.on && !partners && !fusb.partner_type);
    sw.error = 0;
    assert(!fusb301_update_status(&fusb) && sw.role == USB_ROLE_NONE);

    /* Fixed-role boards continue working without a role-switch provider. */
    fusb.role_sw = NULL;
    map.status = 1 | 0x20;
    assert(!fusb301_update_status(&fusb) && vbus.on && partners == 1);
    map.status = 0;
    assert(!fusb301_update_status(&fusb) && !vbus.on && !partners);

    fusb.role_sw = &sw;
    map.status = 1 | 0x20;
    assert(!fusb301_update_status(&fusb) && sw.role == USB_ROLE_HOST);
    irq_live = removing = true;
    fusb301_remove(&client);
    assert(!irq_live && !port_live && switch_released && !partners && !vbus.on);
    assert(sw.role == USB_ROLE_NONE);
    puts("PASS: FUSB301 roles, orientations, detach, provider failures and IRQ teardown");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="fusb301-role-") as temp:
    source_path = Path(temp) / "test.c"
    binary = Path(temp) / "test"
    source_path.write_text(code)
    subprocess.run(shlex.split(os.environ.get("HOSTCC", "cc")) + [
        "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(source_path),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
