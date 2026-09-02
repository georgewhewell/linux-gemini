/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NOVATEK_NT36XXX_H
#define _NOVATEK_NT36XXX_H

#include <linux/types.h>

#define NVT36XXX_HW_ADDR		0x62
#define NVT36XXX_FW_ADDR		0x01
#define NVT36XXX_XDATA_CMD		0xff
#define NVT36XXX_TRIM_ADDR		0x1f600
#define NVT36XXX_TRIM_CMD		0x4e
#define NVT36XXX_TRIM_LEN		6
#define NVT36XXX_EVENT_BUF_ADDR		0x11e00
#define NVT36XXX_EVENT_DATA_LEN		65
#define NVT36XXX_HOST_CMD		0x50
#define NVT36XXX_DEEP_SLEEP		0x11
#define NVT36XXX_FW_INFO_CMD		0x78
#define NVT36XXX_FW_INFO_LEN		16
#define NVT36XXX_RESET_STATE_CMD	0x60
#define NVT36XXX_RESET_STATE_LEN	5
#define NVT36XXX_RESET_STATE_INIT	0xa0
#define NVT36XXX_RESET_STATE_REK	0xa1
#define NVT36XXX_RESET_RETRIES		5
#define NVT36XXX_RESET_STATE_RETRIES	100
#define NVT36XXX_MAX_TOUCHES		10
#define NVT36XXX_MAX_COORD		4096
#define NVT36XXX_MAX_PRESSURE		1000
#define NVT36XXX_MAX_WIDTH		255

struct nvt36xxx_contact {
	unsigned int slot;
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int pressure;
};

static inline bool
nvt36xxx_trim_is_nt36772(const u8 trim[NVT36XXX_TRIM_LEN])
{
	return trim[3] == 0x72 && trim[4] == 0x66 && trim[5] == 0x03;
}

static inline bool
nvt36xxx_decode_contact(const u8 event[NVT36XXX_EVENT_DATA_LEN],
			unsigned int index, struct nvt36xxx_contact *contact)
{
	const u8 *point = &event[index * 6];
	unsigned int status = point[0] & 0x07;

	contact->slot = point[0] >> 3;
	if (!contact->slot || contact->slot > NVT36XXX_MAX_TOUCHES ||
	    (status != 0x01 && status != 0x02))
		return false;

	contact->x = (point[1] << 4) | (point[3] >> 4);
	contact->y = (point[2] << 4) | (point[3] & 0x0f);
	contact->width = point[4] ? point[4] : 1;
	if (index < 2)
		contact->pressure = point[5] | (event[62 + index] << 8);
	else
		contact->pressure = point[5];

	return true;
}

#endif /* _NOVATEK_NT36XXX_H */
