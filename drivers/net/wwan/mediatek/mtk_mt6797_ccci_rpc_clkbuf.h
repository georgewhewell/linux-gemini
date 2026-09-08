/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_RPC_CLKBUF_H
#define _MTK_MT6797_CCCI_RPC_CLKBUF_H

#include "mtk_mt6797_ccci_rpc.h"

#define MT6797_RPC_CLKBUF 0x4008U
#define MT6797_RPC_CLKBUF_REPLY_SIZE 56U

struct mt6797_rpc_clkbuf_values {
	u8 configuration[4];
	u8 software[4];
	u16 driving[4];
};

/* Called in process context only, after complete wire validation. A successful
 * provider must apply/retain the AFC word and supply its actual clock state.
 * No default board values or hardware access belong in this codec.
 */
typedef int (*mt6797_rpc_clkbuf_provider)(void *context, u32 afc,
				struct mt6797_rpc_clkbuf_values *values);

static inline int mt6797_rpc_clkbuf_reply(const u8 *data, size_t len,
		mt6797_rpc_clkbuf_provider provider, void *context,
		u8 *reply, size_t capacity, size_t *reply_len, s32 *result)
{
	struct mt6797_rpc_clkbuf_values values = {};
	const u8 *arg = NULL;
	u32 argc, index, length, first_length = 0, count = 0;
	size_t cursor = 24, padded, size = 40;
	s32 status = -2; /* vendor FS_PARAM_ERROR */
	unsigned int i;
	int ret;

	/* Reserve the largest response before invoking any provider effects. */
	if (!data || !reply || !reply_len || !result || len < 24 ||
	    len > 2072 || capacity < MT6797_RPC_CLKBUF_REPLY_SIZE)
		return -EMSGSIZE;
	if ((mt6797_rpc_read(data + 8) & 0xffff) != MT6797_RPC_RX ||
	    mt6797_rpc_read(data + 4) != len)
		return -EPROTO;
	index = mt6797_rpc_read(data + 12);
	if (index >= 2)
		return -EPROTO;
	if (mt6797_rpc_read(data + 16) != MT6797_RPC_CLKBUF)
		return -EOPNOTSUPP;
	argc = mt6797_rpc_read(data + 20);
	if (argc > 6)
		return -EPROTO;
	for (i = 0; i < argc; i++) {
		if (len - cursor < 4)
			return -EMSGSIZE;
		length = mt6797_rpc_read(data + cursor);
		cursor += 4;
		if (length > 2048)
			return -EMSGSIZE;
		padded = (length + 3) & ~3U;
		if (padded > len - cursor)
			return -EMSGSIZE;
		if (!i) {
			arg = data + cursor;
			first_length = length;
		}
		cursor += padded;
	}
	if (cursor != len)
		return -EPROTO;
	/* MT6797 enables MD_UMOLY_EE_SUPPORT: packed u16 count + u32 AFC. */
	if (argc != 1 || first_length != 6)
		goto respond;
	count = arg[0] | ((u32)arg[1] << 8);
	if (count != 4) {
		/* Vendor count-mismatch result; initialize all otherwise-unused bytes. */
		count = 0xff;
		status = 0;
		size = MT6797_RPC_CLKBUF_REPLY_SIZE;
		goto respond;
	}
	if (!provider)
		return -EOPNOTSUPP;
	ret = provider(context, mt6797_rpc_read(arg + 2), &values);
	status = -5; /* vendor FS_FUNC_FAIL; provider failure is not success */
	if (ret)
		goto respond;
	for (i = 0; i < 4; i++)
		if (values.configuration[i] > 2 || values.software[i] > 1 ||
		    values.driving[i] > 3)
			goto respond;
	status = 0;
	size = MT6797_RPC_CLKBUF_REPLY_SIZE;
respond:
	memset(reply, 0, size);
	memcpy(reply, data, 4);
	mt6797_rpc_write(reply + 4, size);
	mt6797_rpc_write(reply + 8, MT6797_RPC_TX);
	mt6797_rpc_write(reply + 12, index);
	mt6797_rpc_write(reply + 16, MT6797_RPC_CLKBUF | 0xffff0000U);
	mt6797_rpc_write(reply + 20, 2);
	mt6797_rpc_write(reply + 24, 4);
	mt6797_rpc_write(reply + 28, status);
	if (size == 40) {
		mt6797_rpc_write(reply + 32, 4);
		mt6797_rpc_write(reply + 36, status);
	} else {
		/* Packed 18-byte vendor result, followed by two zero alignment bytes. */
		mt6797_rpc_write(reply + 32, 18);
		reply[36] = count;
		reply[37] = count >> 8;
		for (i = 0; i < 4; i++) {
			reply[38 + i] = values.configuration[i];
			reply[42 + i] = values.software[i];
			reply[46 + 2 * i] = values.driving[i];
			reply[47 + 2 * i] = values.driving[i] >> 8;
		}
	}
	*reply_len = size;
	*result = status;
	return 0;
}
#endif
