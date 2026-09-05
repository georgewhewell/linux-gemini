/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_CCCI_COMMON_H
#define __MTK_CCCI_COMMON_H

#include <linux/types.h>

#define MTK_CCCI_HEADER_SIZE		16U
#define MTK_CCCI_FEATURE_COUNT		64U
#define MTK_CCCI_FEATURE_QUERY_SIZE	72U
#define MTK_CCCI_RUNTIME_V2_RESPONSE_SIZE 100U
#define MTK_CCCI_RUNTIME_V2_REPLY_SIZE \
	(MTK_CCCI_HEADER_SIZE + MTK_CCCI_RUNTIME_V2_RESPONSE_SIZE)
#define MTK_CCCI_RUNTIME_AP_SIZE	0x800U
#define MTK_CCCI_RUNTIME_MD_SIZE	0x800U
#define MTK_CCCI_RUNTIME_OFFSET		0x10000U
#define MTK_CCCI_EXCEPTION_SIZE		0x10000U
#define MTK_CCCI_NET_MTU		1500U
#define MTK_CCCI_NET_RX_SIZE		(3584U - MTK_CCCI_HEADER_SIZE)
#define MTK_CCCI_MAX_NET_LINKS		18U

#define MTK_CCCI_MD_FEATURE_PATTERN	0x49434343U
#define MTK_CCCI_AP_FEATURE_PATTERN	0x43434349U
#define MTK_CCCI_MD_INIT_CHK_ID		0x5555ffffU

#define MTK_CCCI_CONTROL_RX		0U
#define MTK_CCCI_CONTROL_TX		1U
#define MTK_CCCI_UART2_RX		10U
#define MTK_CCCI_UART2_TX		12U

enum mtk_ccci_feature_support {
	MTK_CCCI_FEATURE_NOT_EXIST,
	MTK_CCCI_FEATURE_NOT_SUPPORT,
	MTK_CCCI_FEATURE_MUST_SUPPORT,
	MTK_CCCI_FEATURE_OPTIONAL_SUPPORT,
	MTK_CCCI_FEATURE_BACKWARD_COMPAT,
};

enum mtk_ccci_runtime_feature_id {
	MTK_CCCI_RT_BOOT_INFO,
	MTK_CCCI_RT_EXCEPTION_SHM,
	MTK_CCCI_RT_CCIF_SHM,
	MTK_CCCI_RT_SMART_LOGGING_SHM,
	MTK_CCCI_RT_MD1_MD3_SHM,
	MTK_CCCI_RT_HIF_DMA_REMAP,
	MTK_CCCI_RT_RTC_32K_LESS,
	MTK_CCCI_RT_RANDOM_SEED,
	MTK_CCCI_RT_GPS_COCLOCK,
	MTK_CCCI_RT_SBP_ID,
	MTK_CCCI_RT_CCCI,
	MTK_CCCI_RT_CLIB_TIME,
	MTK_CCCI_RT_C2K,
	MTK_CCCI_RT_MD_IMAGE_START,
	MTK_CCCI_RT_CCISM_SHM,
	MTK_CCCI_RT_CCB_SHM,
	MTK_CCCI_RT_DHL_RAW_SHM,
	MTK_CCCI_RT_DT_NETD_SHM,
	MTK_CCCI_RT_DT_USB_SHM,
	MTK_CCCI_RT_EE_AFTER_EPOF,
	MTK_CCCI_RT_CCMNI_MTU,
	MTK_CCCI_RT_CUSTOMER = 21,
	MTK_CCCI_RT_FAST_HEADER,
	MTK_CCCI_RT_RESERVED_23,
	MTK_CCCI_RT_C2K_MEID = 24,
	MTK_CCCI_RT_FEATURE_COUNT,
};

struct mtk_ccci_header {
	__le32 data0;
	__le32 data1;
	__le32 status;
	__le32 reserved;
} __packed;

struct mtk_ccci_frame {
	u32 data0;
	u32 data1;
	u32 reserved;
	u16 channel;
	u16 sequence;
	bool assert_bit;
};

struct mtk_ccci_runtime_config {
	u32 smem_md_addr;
	u32 smem_size;
	u32 random_seed;
	u32 ccmni_mtu;
};

void mtk_ccci_header_init(struct mtk_ccci_header *header, u16 channel,
			  u16 sequence, size_t frame_len);
int mtk_ccci_header_decode(const void *data, size_t len,
			   struct mtk_ccci_frame *frame);
int mtk_ccci_net_channels(u32 if_id, u16 *tx_channel, u16 *rx_channel,
			  u8 *tx_queue);
int mtk_ccci_net_rx_link(u16 channel);
int mtk_ccci_runtime_v2_build(const void *query, size_t query_len,
			      const struct mtk_ccci_runtime_config *config,
			      void *runtime, size_t runtime_size,
			      size_t *runtime_len);
int mtk_ccci_runtime_v2_response(const struct mtk_ccci_runtime_config *config,
				 void *response, size_t response_size,
				 size_t *response_len);
int mtk_ccci_runtime_v2_reply(const void *hs1, size_t hs1_len,
			      const struct mtk_ccci_runtime_config *config,
			      u16 sequence, void *runtime, size_t runtime_size,
			      size_t *runtime_len, void *reply, size_t reply_size,
			      size_t *reply_len);

#endif /* __MTK_CCCI_COMMON_H */
