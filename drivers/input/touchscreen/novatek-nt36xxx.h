/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NOVATEK_NT36XXX_H
#define _NOVATEK_NT36XXX_H

#include <linux/kernel.h>
#include <linux/types.h>

#define NVT36XXX_HW_ADDR		0x62
#define NVT36XXX_FW_ADDR		0x01
#define NVT36XXX_XDATA_CMD		0xff
#define NVT36XXX_TRIM_CMD		0x4e
#define NVT36XXX_TRIM_LEN		6
#define NVT36XXX_EVENT_DATA_LEN	65
#define NVT36XXX_FW_INFO_CMD		0x78
#define NVT36XXX_FW_INFO_LEN		16
#define NVT36XXX_RESET_STATE_CMD	0x60
#define NVT36XXX_RESET_STATE_LEN	5
#define NVT36XXX_MAX_TOUCHES		10
#define NVT36XXX_MAX_COORD		4096
#define NVT36XXX_MAX_PRESSURE		1000
#define NVT36XXX_MAX_WIDTH		255
#define NVT36XXX_RESET_RETRIES		5
#define NVT36XXX_RESET_STATE_INIT	0xa0

struct nvt36xxx_mem_map {
	u32 event_buf_addr;
};

struct nvt36xxx_chip {
	const char *name;
	u8 id[NVT36XXX_TRIM_LEN];
	u8 mask[NVT36XXX_TRIM_LEN];
	const struct nvt36xxx_mem_map *mem_map;
};

/*
 * These trim masks and xdata maps are the complete set in the Gemini vendor
 * NT36xxx driver. The trim read, rather than the compatible string, selects
 * the map used for all later firmware accesses.
 */
static const struct nvt36xxx_mem_map nvt36xxx_nt36772_mem_map = {
	.event_buf_addr = 0x11e00,
};

static const struct nvt36xxx_mem_map nvt36xxx_nt36525_mem_map = {
	.event_buf_addr = 0x11a00,
};

static const struct nvt36xxx_mem_map nvt36xxx_nt36870_mem_map = {
	.event_buf_addr = 0x25000,
};

static const struct nvt36xxx_mem_map nvt36xxx_nt36676f_mem_map = {
	.event_buf_addr = 0x11a00,
};

static const struct nvt36xxx_chip nvt36xxx_chips[] = {
	{
		.name = "NT36772",
		.id = { 0x55, 0x00, 0xff, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0x55, 0x72, 0xff, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xaa, 0x00, 0xff, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xaa, 0x72, 0xff, 0x00, 0x00, 0x00 },
		.mask = { 1, 1, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xff, 0xff, 0xff, 0x72, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xff, 0xff, 0xff, 0x70, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xff, 0xff, 0xff, 0x70, 0x67, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36772",
		.id = { 0xff, 0xff, 0xff, 0x72, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36772_mem_map,
	}, {
		.name = "NT36525",
		.id = { 0xff, 0xff, 0xff, 0x25, 0x65, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36525_mem_map,
	}, {
		.name = "NT36870",
		.id = { 0xff, 0xff, 0xff, 0x70, 0x68, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36870_mem_map,
	}, {
		.name = "NT36676F",
		.id = { 0xff, 0xff, 0xff, 0x76, 0x66, 0x03 },
		.mask = { 0, 0, 0, 1, 1, 1 },
		.mem_map = &nvt36xxx_nt36676f_mem_map,
	},
};

static inline const struct nvt36xxx_chip *
nvt36xxx_find_chip(const u8 trim[NVT36XXX_TRIM_LEN])
{
	const struct nvt36xxx_chip *chip;
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(nvt36xxx_chips); i++) {
		chip = &nvt36xxx_chips[i];
		for (j = 0; j < NVT36XXX_TRIM_LEN; j++)
			if (chip->mask[j] && trim[j] != chip->id[j])
				break;
		if (j == NVT36XXX_TRIM_LEN)
			return chip;
	}

	return NULL;
}

struct nvt36xxx_contact {
	unsigned int slot;
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int pressure;
};

static inline bool
nvt36xxx_decode_contact(const u8 event[NVT36XXX_EVENT_DATA_LEN],
			unsigned int contact, struct nvt36xxx_contact *decoded)
{
	const u8 *point = &event[contact * 6];
	unsigned int status = point[0] & 0x07;

	decoded->slot = point[0] >> 3;
	if (!decoded->slot || decoded->slot > NVT36XXX_MAX_TOUCHES ||
	    (status != 0x01 && status != 0x02))
		return false;

	decoded->x = (point[1] << 4) | (point[3] >> 4);
	decoded->y = (point[2] << 4) | (point[3] & 0x0f);
	decoded->width = point[4] ? point[4] : 1;
	if (contact < 2)
		decoded->pressure = point[5] | (event[62 + contact] << 8);
	else
		decoded->pressure = point[5];

	return true;
}

#endif /* _NOVATEK_NT36XXX_H */
