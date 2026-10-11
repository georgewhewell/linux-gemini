// SPDX-License-Identifier: GPL-2.0-only
/* MediaTek CCCI wire helpers shared by integrated-modem WWAN drivers. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "mtk_ccci_common.h"

#define MTK_CCCI_STATUS_ASSERT		BIT(31)
#define MTK_CCCI_STATUS_SEQ_SHIFT	16
#define MTK_CCCI_STATUS_SEQ_MASK	GENMASK(30, 16)
#define MTK_CCCI_STATUS_CHANNEL_MASK	GENMASK(15, 0)

#define MTK_CCCI_RUNTIME_RECORD_SIZE	8U
#define MTK_CCCI_MISC_RECORD_SIZE	16U
#define MTK_CCCI_BOOT_RECORD_SIZE	16U
#define MTK_CCCI_SHM_RECORD_SIZE	8U

struct mtk_ccci_feature_query {
	__le32 head_pattern;
	u8 feature[MTK_CCCI_FEATURE_COUNT];
	__le32 tail_pattern;
} __packed;

struct mtk_ccci_feature_response {
	__le32 head_pattern;
	u8 feature[MTK_CCCI_FEATURE_COUNT];
	__le32 share_memory_support;
	__le32 ap_runtime_data_addr;
	__le32 ap_runtime_data_size;
	__le32 md_runtime_data_addr;
	__le32 md_runtime_data_size;
	__le32 set_md_mpu_start_addr;
	__le32 set_md_mpu_total_size;
	__le32 tail_pattern;
} __packed;

static_assert(sizeof(struct mtk_ccci_header) == MTK_CCCI_HEADER_SIZE);
static_assert(sizeof(struct mtk_ccci_feature_query) ==
	      MTK_CCCI_FEATURE_QUERY_SIZE);
static_assert(sizeof(struct mtk_ccci_feature_response) ==
	      MTK_CCCI_RUNTIME_V2_RESPONSE_SIZE);

static u8 mtk_ccci_feature(u8 support, u8 version)
{
	return (version << 4) | support;
}

static u8 mtk_ccci_feature_support(u8 feature)
{
	return feature & 0xf;
}

static u8 mtk_ccci_feature_version(u8 feature)
{
	return feature >> 4;
}

void mtk_ccci_header_init(struct mtk_ccci_header *header, u16 channel,
			  u16 sequence, size_t frame_len)
{
	u32 status;

	status = channel | ((sequence & 0x7fff) << MTK_CCCI_STATUS_SEQ_SHIFT) |
		 MTK_CCCI_STATUS_ASSERT;
	header->data0 = 0;
	header->data1 = cpu_to_le32(frame_len);
	header->status = cpu_to_le32(status);
	header->reserved = 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_header_init);

int mtk_ccci_header_decode(const void *data, size_t len,
			   struct mtk_ccci_frame *frame)
{
	const struct mtk_ccci_header *header = data;
	u32 status;

	if (!data || !frame || len < sizeof(*header))
		return -EMSGSIZE;

	status = le32_to_cpu(header->status);
	frame->data0 = le32_to_cpu(header->data0);
	frame->data1 = le32_to_cpu(header->data1);
	frame->channel = status & MTK_CCCI_STATUS_CHANNEL_MASK;
	frame->sequence = (status & MTK_CCCI_STATUS_SEQ_MASK) >>
			  MTK_CCCI_STATUS_SEQ_SHIFT;
	frame->assert_bit = status & MTK_CCCI_STATUS_ASSERT;
	frame->reserved = le32_to_cpu(header->reserved);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_header_decode);

int mtk_ccci_net_channels(u32 if_id, u16 *tx_channel, u16 *rx_channel,
			  u8 *tx_queue)
{
	u16 tx, rx;
	u8 queue;

	if (if_id == 8 || if_id >= MTK_CCCI_MAX_NET_LINKS)
		return -EINVAL;

	if (if_id < 3) {
		rx = 20 + 4 * if_id;
		tx = rx + 2;
	} else if (if_id < 8) {
		rx = 69 + 5 * (if_id - 3);
		tx = rx + 2;
	} else {
		rx = 103 + 5 * (if_id - 9);
		tx = rx + 2;
	}
	queue = if_id < 2 || if_id == 7 ? 3 : 5;

	if (tx_channel)
		*tx_channel = tx;
	if (rx_channel)
		*rx_channel = rx;
	if (tx_queue)
		*tx_queue = queue;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_net_channels);

int mtk_ccci_net_rx_link(u16 channel)
{
	u32 if_id;
	u16 rx;

	for (if_id = 0; if_id < MTK_CCCI_MAX_NET_LINKS; if_id++) {
		if (!mtk_ccci_net_channels(if_id, NULL, &rx, NULL) && rx == channel)
			return if_id;
	}
	return -ENOENT;
}
EXPORT_SYMBOL_GPL(mtk_ccci_net_rx_link);

/* The query and runtime stream have 64 wire slots, not just the currently
 * named feature IDs. Unnamed slots remain NOT_EXIST on the AP side; required
 * unknown features must fail negotiation rather than be silently ignored.
 */
static const u8 mtk_ccci_ap_features[MTK_CCCI_FEATURE_COUNT] = {
	[MTK_CCCI_RT_BOOT_INFO] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_EXCEPTION_SHM] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_CCIF_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_SMART_LOGGING_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_MD1_MD3_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_HIF_DMA_REMAP] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_RTC_32K_LESS] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_RANDOM_SEED] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_GPS_COCLOCK] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_SBP_ID] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_CCCI] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_CLIB_TIME] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_C2K] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_MD_IMAGE_START] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_CCISM_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_CCB_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_DHL_RAW_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_DT_NETD_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_DT_USB_SHM] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_EE_AFTER_EPOF] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_CCMNI_MTU] =
		MTK_CCCI_FEATURE_MUST_SUPPORT,
	[MTK_CCCI_RT_CUSTOMER] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[MTK_CCCI_RT_FAST_HEADER] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
	[23] = MTK_CCCI_FEATURE_NOT_EXIST,
	[MTK_CCCI_RT_C2K_MEID] =
		MTK_CCCI_FEATURE_NOT_SUPPORT,
};

static int mtk_ccci_select_feature(u8 modem, u8 ap, u8 *selected)
{
	u8 modem_support = mtk_ccci_feature_support(modem);
	u8 modem_version = mtk_ccci_feature_version(modem);
	u8 ap_support = mtk_ccci_feature_support(ap);
	u8 ap_version = mtk_ccci_feature_version(ap);

	switch (modem_support) {
	case MTK_CCCI_FEATURE_NOT_EXIST:
		*selected = modem;
		break;
	case MTK_CCCI_FEATURE_MUST_SUPPORT:
		if (ap_support < MTK_CCCI_FEATURE_MUST_SUPPORT)
			return -EOPNOTSUPP;
		*selected = modem;
		break;
	case MTK_CCCI_FEATURE_OPTIONAL_SUPPORT:
		if (ap_support >= MTK_CCCI_FEATURE_MUST_SUPPORT &&
		    modem_version == ap_version)
			*selected = mtk_ccci_feature(MTK_CCCI_FEATURE_MUST_SUPPORT, ap_version);
		else
			*selected = mtk_ccci_feature(MTK_CCCI_FEATURE_NOT_SUPPORT, ap_version);
		break;
	case MTK_CCCI_FEATURE_BACKWARD_COMPAT:
		if (ap_support >= MTK_CCCI_FEATURE_MUST_SUPPORT &&
		    modem_version >= ap_version)
			*selected = mtk_ccci_feature(MTK_CCCI_FEATURE_MUST_SUPPORT, ap_version);
		else
			*selected = mtk_ccci_feature(MTK_CCCI_FEATURE_NOT_SUPPORT, ap_version);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mtk_ccci_runtime_append(u8 **cursor, const u8 *end, u8 id,
				   u8 selected, const void *data, u32 len)
{
	u8 *record = *cursor;

	if (record > end || len > MTK_CCCI_MISC_RECORD_SIZE ||
	    (size_t)(end - record) < MTK_CCCI_RUNTIME_RECORD_SIZE + len)
		return -ENOSPC;
	record[0] = id;
	record[1] = selected;
	record[2] = 0;
	record[3] = 0;
	put_unaligned_le32(len, record + 4);
	if (len)
		memcpy(record + MTK_CCCI_RUNTIME_RECORD_SIZE, data, len);
	*cursor += MTK_CCCI_RUNTIME_RECORD_SIZE + len;
	return 0;
}

int mtk_ccci_runtime_v2_build(const void *query, size_t query_len,
			      const struct mtk_ccci_runtime_config *config,
			      void *runtime, size_t runtime_size,
			      size_t *runtime_len)
{
	const struct mtk_ccci_feature_query *modem = query;
	u8 boot[MTK_CCCI_BOOT_RECORD_SIZE] = {};
	u8 misc[MTK_CCCI_MISC_RECORD_SIZE] = {};
	u8 shm[MTK_CCCI_SHM_RECORD_SIZE] = {};
	u8 *cursor = runtime;
	u8 selected;
	unsigned int id;
	int ret;

	if (!query || !config || !runtime || !runtime_len ||
	    query_len != sizeof(*modem) ||
	    runtime_size < MTK_CCCI_RUNTIME_AP_SIZE)
		return -EINVAL;
	if (le32_to_cpu(modem->head_pattern) != MTK_CCCI_MD_FEATURE_PATTERN ||
	    le32_to_cpu(modem->tail_pattern) != MTK_CCCI_MD_FEATURE_PATTERN)
		return -EBADMSG;

	for (id = 0; id < MTK_CCCI_FEATURE_COUNT; id++) {
		u8 ap = mtk_ccci_feature(mtk_ccci_ap_features[id], 0);
		const void *data = NULL;
		u32 len = 0;

		ret = mtk_ccci_select_feature(modem->feature[id], ap, &selected);
		if (ret)
			return ret;
		memset(misc, 0, sizeof(misc));
		if (mtk_ccci_feature_support(selected) ==
		    MTK_CCCI_FEATURE_MUST_SUPPORT) {
			switch (id) {
			case MTK_CCCI_RT_BOOT_INFO:
				put_unaligned_le32(MTK_CCCI_CONTROL_RX, boot);
				data = boot;
				len = sizeof(boot);
				break;
			case MTK_CCCI_RT_EXCEPTION_SHM:
				put_unaligned_le32(config->smem_md_addr, shm);
				put_unaligned_le32(MTK_CCCI_EXCEPTION_SIZE,
						   shm + sizeof(u32));
				data = shm;
				len = sizeof(shm);
				break;
			case MTK_CCCI_RT_RANDOM_SEED:
				put_unaligned_le32(config->random_seed, misc);
				fallthrough;
			case MTK_CCCI_RT_HIF_DMA_REMAP:
			case MTK_CCCI_RT_GPS_COCLOCK:
			case MTK_CCCI_RT_EE_AFTER_EPOF:
				data = misc;
				len = sizeof(misc);
				break;
			case MTK_CCCI_RT_SBP_ID: {
				u32 bitmap = 0;

				if (!config->load_type ||
				    config->load_type > MTK_CCCI_MAX_LOAD_TYPE)
					return -ENODATA;
				if (config->load_type >= MTK_CCCI_UBIN_FIRST_TYPE) {
					if (!config->modem_capability_known)
						return -ENODATA;
					ret = mtk_ccci_ubin_wm_bitmap(
						config->modem_capability, &bitmap);
					if (ret)
						return ret;
				}
				/* SBP defaults to zero; only UBIN publishes the WM word. */
				put_unaligned_le32(bitmap, misc + sizeof(u32));
				data = misc;
				len = sizeof(misc);
				break;
			}
			case MTK_CCCI_RT_CCCI:
				put_unaligned_le32(BIT(0), misc);
				data = misc;
				len = sizeof(misc);
				break;
			case MTK_CCCI_RT_CCMNI_MTU:
				put_unaligned_le32(config->ccmni_mtu, misc);
				data = misc;
				len = sizeof(u32);
				break;
			default:
				return -EOPNOTSUPP;
			}
		}

		ret = mtk_ccci_runtime_append(&cursor,
					      (u8 *)runtime + runtime_size,
					      id, selected, data, len);
		if (ret)
			return ret;
	}
	*runtime_len = cursor - (u8 *)runtime;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_runtime_v2_build);

int mtk_ccci_runtime_v2_response(const struct mtk_ccci_runtime_config *config,
				 void *response, size_t response_size,
				 size_t *response_len)
{
	struct mtk_ccci_feature_response *ap = response;
	u64 ap_runtime_addr;
	u64 md_runtime_addr;

	if (!config || !response || !response_len ||
	    response_size < sizeof(*ap) ||
	    config->smem_size < MTK_CCCI_RUNTIME_OFFSET +
				MTK_CCCI_RUNTIME_AP_SIZE + MTK_CCCI_RUNTIME_MD_SIZE ||
	    config->md_mpu_smem_size < config->smem_size)
		return -EINVAL;
	ap_runtime_addr = (u64)config->smem_md_addr + MTK_CCCI_RUNTIME_OFFSET;
	md_runtime_addr = ap_runtime_addr +
			  MTK_CCCI_RUNTIME_AP_SIZE;
	if (ap_runtime_addr > U32_MAX || md_runtime_addr > U32_MAX ||
	    (u64)config->smem_md_addr + config->md_mpu_smem_size - 1 > U32_MAX)
		return -ERANGE;

	memset(ap, 0, sizeof(*ap));
	ap->head_pattern = cpu_to_le32(MTK_CCCI_AP_FEATURE_PATTERN);
	/* Vendor marks feature slot 1 as version 1 but not supported here. */
	ap->feature[1] = mtk_ccci_feature(MTK_CCCI_FEATURE_NOT_EXIST, 1);
	ap->share_memory_support = cpu_to_le32(1);
	ap->ap_runtime_data_addr = cpu_to_le32(ap_runtime_addr);
	ap->ap_runtime_data_size = cpu_to_le32(MTK_CCCI_RUNTIME_AP_SIZE);
	ap->md_runtime_data_addr = cpu_to_le32(md_runtime_addr);
	ap->md_runtime_data_size = cpu_to_le32(MTK_CCCI_RUNTIME_MD_SIZE);
	ap->set_md_mpu_start_addr = cpu_to_le32(config->smem_md_addr);
	ap->set_md_mpu_total_size = cpu_to_le32(config->md_mpu_smem_size);
	ap->tail_pattern = cpu_to_le32(MTK_CCCI_AP_FEATURE_PATTERN);
	*response_len = sizeof(*ap);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_runtime_v2_response);

int mtk_ccci_runtime_v2_reply(const void *hs1, size_t hs1_len,
			      const struct mtk_ccci_runtime_config *config,
			      u16 sequence, void *runtime, size_t runtime_size,
			      size_t *runtime_len, void *reply, size_t reply_size,
			      size_t *reply_len)
{
	struct mtk_ccci_header *reply_header = reply;
	struct mtk_ccci_frame frame;
	size_t feature_len;
	int ret;

	if (!hs1 || !config || !runtime || !runtime_len || !reply || !reply_len ||
	    hs1_len != MTK_CCCI_HEADER_SIZE + MTK_CCCI_FEATURE_QUERY_SIZE ||
	    reply_size < MTK_CCCI_RUNTIME_V2_REPLY_SIZE)
		return -EINVAL;
	ret = mtk_ccci_header_decode(hs1, hs1_len, &frame);
	if (ret)
		return ret;
	if (frame.channel != MTK_CCCI_CONTROL_RX || frame.data1 ||
	    frame.reserved != MTK_CCCI_MD_INIT_CHK_ID)
		return -ENOMSG;

	ret = mtk_ccci_runtime_v2_build((const u8 *)hs1 + MTK_CCCI_HEADER_SIZE,
					MTK_CCCI_FEATURE_QUERY_SIZE, config,
					runtime, runtime_size, runtime_len);
	if (ret)
		return ret;
	ret = mtk_ccci_runtime_v2_response(config,
					   (u8 *)reply + MTK_CCCI_HEADER_SIZE,
					   reply_size - MTK_CCCI_HEADER_SIZE,
					   &feature_len);
	if (ret)
		return ret;

	mtk_ccci_header_init(reply_header, MTK_CCCI_CONTROL_TX, sequence,
			     MTK_CCCI_HEADER_SIZE + feature_len);
	reply_header->reserved = cpu_to_le32(MTK_CCCI_MD_INIT_CHK_ID);
	*reply_len = MTK_CCCI_HEADER_SIZE + feature_len;
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_ccci_runtime_v2_reply);

/*
 * Public MT6797 ccci_util_lib_fo.c, 8cfe6596: capability is an option,
 * independent of hdr_tbl_inf.md_type and md1_chk.image_type. AP RAT bits
 * differ from the MD bitmap sent in MISC_INFO_SBP_ID.
 */
enum mtk_ccci_ap_rat {
	MD1_RAT_GSM = BIT(0),
	MD1_RAT_TDS = BIT(1),
	MD1_RAT_WCDMA = BIT(2),
	MD1_RAT_TDD_LTE = BIT(3),
	MD1_RAT_FDD_LTE = BIT(4),
	MD1_RAT_CDMA2000 = BIT(5),
};

static const u8 mtk_ccci_legacy_ubin_rat[] = {
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_WCDMA |
		MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_TDD_LTE |
		MD1_RAT_FDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_WCDMA |
		MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_TDD_LTE,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_FDD_LTE,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_FDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_FDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_TDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_TDD_LTE,
	MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_TDD_LTE | MD1_RAT_CDMA2000,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_FDD_LTE,
	MD1_RAT_GSM | MD1_RAT_TDS | MD1_RAT_FDD_LTE | MD1_RAT_CDMA2000,
};

int mtk_ccci_ubin_wm_bitmap(s32 capability, u32 *bitmap)
{
	u32 rat, result = 0;

	if (!bitmap)
		return -EINVAL;
	if (capability <= 0)
		return -ENODATA;
	if ((capability & 0x5a000000U) == 0x5a000000U)
		rat = capability & 0x3f;
	else if (capability < MTK_CCCI_UBIN_FIRST_TYPE)
		rat = 0;
	else if (capability <= MTK_CCCI_MAX_LOAD_TYPE)
		rat = mtk_ccci_legacy_ubin_rat[
			capability - MTK_CCCI_UBIN_FIRST_TYPE];
	else
		return -EOPNOTSUPP;

	if (rat == (MD1_RAT_GSM | MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE |
		    MD1_RAT_CDMA2000) ||
	    rat == (MD1_RAT_GSM | MD1_RAT_WCDMA | MD1_RAT_CDMA2000))
		rat |= MD1_RAT_WCDMA | MD1_RAT_TDD_LTE | MD1_RAT_FDD_LTE;

	if (rat & MD1_RAT_GSM)
		result |= BIT(0);
	if (rat & MD1_RAT_TDS)
		result |= BIT(1);
	if (rat & MD1_RAT_CDMA2000)
		result |= BIT(2);
	if (rat & MD1_RAT_WCDMA)
		result |= BIT(3);
	if (rat & MD1_RAT_TDD_LTE)
		result |= BIT(4);
	if (rat & MD1_RAT_FDD_LTE)
		result |= BIT(5);
	*bitmap = result;
	return 0;
}

EXPORT_SYMBOL_GPL(mtk_ccci_ubin_wm_bitmap);
