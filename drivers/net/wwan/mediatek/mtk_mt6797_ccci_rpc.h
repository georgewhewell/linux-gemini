/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_RPC_H
#define _MTK_MT6797_CCCI_RPC_H

/* Vendor eccci/port_rpc.{c,h}; no allocation, MMIO or pin control here. */
#define MT6797_RPC_RX 32U
#define MT6797_RPC_TX 33U
#define MT6797_RPC_EINT 0x4005U
#define MT6797_RPC_REPLY_SIZE 40U

struct mt6797_rpc_eint {
	bool present;
	u8 valid;
	u32 value[7]; /* number, debounce ms, polarity, edge, socket, dedicated, selector */
};

static bool mt6797_rpc_eint_value(unsigned int type, u32 raw, u32 *value)
{
	if (type >= 7 || !value ||
	    ((type == 0 || type == 4) && raw > 255) ||
	    ((type == 2 || type == 3 || type == 5) && raw > 1) ||
	    (type == 6 && raw > 2))
		return false;
	*value = type == 1 ? raw / 1000 : raw; /* vendor AP us -> MD ms */
	return true;
}

static u32 mt6797_rpc_read(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void mt6797_rpc_write(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

static int mt6797_rpc_eint_reply(const u8 *data, size_t len,
		const struct mt6797_rpc_eint sim[2], u8 *reply, size_t capacity,
		s32 *result)
{
	static const char * const names[] = {
		"MD1_SIM1_HOT_PLUG_EINT", "MD1_SIM2_HOT_PLUG_EINT",
	};
	const u8 *arg[6];
	u32 lengths[6], argc, op, index, type, value;
	size_t cursor, padded;
	unsigned int i;
	s32 status = -2; /* vendor FS_PARAM_ERROR */

	if (!data || !sim || !reply || !result || len < 24 || len > 2072 ||
	    capacity < MT6797_RPC_REPLY_SIZE)
		return -EMSGSIZE;
	if ((mt6797_rpc_read(data + 8) & 0xffff) != MT6797_RPC_RX ||
	    mt6797_rpc_read(data + 4) != len)
		return -EPROTO;
	index = mt6797_rpc_read(data + 12);
	/* Two request slots. Vendor's >2 check is an off-by-one; do not copy it. */
	if (index >= 2)
		return -EPROTO;
	op = mt6797_rpc_read(data + 16);
	argc = mt6797_rpc_read(data + 20);
	if (op != MT6797_RPC_EINT)
		return -EOPNOTSUPP;
	if (argc > 6)
		return -EPROTO;
	cursor = 24;
	for (i = 0; i < argc; i++) {
		if (len - cursor < 4)
			return -EMSGSIZE;
		lengths[i] = mt6797_rpc_read(data + cursor);
		cursor += 4;
		if (lengths[i] > 2048)
			return -EMSGSIZE;
		padded = (lengths[i] + 3) & ~3U;
		if (padded > len - cursor)
			return -EMSGSIZE;
		arg[i] = data + cursor;
		cursor += padded;
	}
	if (cursor != len)
		return -EPROTO;
	if (argc != 3 || lengths[0] < 2 || lengths[0] > 64 ||
	    lengths[1] != 4 || lengths[2] != 4)
		goto respond;
	if (mt6797_rpc_read(arg[1]) != lengths[0] || arg[0][lengths[0] - 1] ||
	    memchr(arg[0], 0, lengths[0] - 1))
		goto respond;
	type = mt6797_rpc_read(arg[2]);
	status = -12; /* ERR_SIM_HOT_PLUG_QUERY_TYPE */
	if (type >= 7)
		goto respond;
	status = -11; /* ERR_SIM_HOT_PLUG_QUERY_STRING */
	for (i = 0; i < 2; i++) {
		if (lengths[0] != strlen(names[i]) + 1 ||
		    memcmp(arg[0], names[i], lengths[0]))
			continue;
		if (!sim[i].present)
			goto respond;
		status = -12;
		if (!(sim[i].valid & (1U << type)))
			goto respond;
		status = 0;
		value = sim[i].value[type];
		goto respond;
	}
respond:
	if (status)
		value = status;
	memset(reply, 0, MT6797_RPC_REPLY_SIZE);
	/* TX owns sequence/assertion; preserve data0 and request index. */
	memcpy(reply, data, 4);
	mt6797_rpc_write(reply + 4, MT6797_RPC_REPLY_SIZE);
	mt6797_rpc_write(reply + 8, MT6797_RPC_TX);
	mt6797_rpc_write(reply + 12, index);
	mt6797_rpc_write(reply + 16, MT6797_RPC_EINT | 0xffff0000U);
	mt6797_rpc_write(reply + 20, 2);
	mt6797_rpc_write(reply + 24, 4);
	mt6797_rpc_write(reply + 28, status);
	mt6797_rpc_write(reply + 32, 4);
	mt6797_rpc_write(reply + 36, value);
	*result = status;
	return 0;
}
#endif
