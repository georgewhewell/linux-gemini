// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bounded MT6797 WMT patch-download controller.
 *
 * The retained Gemini trace establishes this exact request order:
 * WMT_SOC.cfg, ROMv3_patch_1_1_hdr.bin, ROMv3_patch_1_0_hdr.bin. It also
 * records the complete-image SHA-256 digests. See the Gemini evidence packet
 * at nixos-gemini/research/mt6797-btif-runtime-evidence-next/
 * firmware-provenance.md:17-23,67-70.
 *
 * Vendor MT6797 code initially uses BTIF mandatory STP to query the chip, then
 * switches the host to BTIF full STP before entering the patch-download loop:
 * gemini-linux-kernel-3.18/drivers/misc/mediatek/connectivity/common/
 * common_main/core/wmt_core.c:755-775,795-809 and
 * common_main/core/wmt_ic_soc.c:1008-1051,1068-1103. For MT6797 (0x0279),
 * the active envelope sends three power-on DLM commands, four MCU-clock-up
 * commands, then the patch loop, followed by three MCU-clock-down commands.
 * The vendor patch loop removes the 28-byte WMT_PATCH header, issues the full
 * and partial patch-address commands, then sends 1000-byte body fragments with
 * first/middle/last tags. It checks the exact address and patch replies, then
 * runs init_table_3's WMT reset after every patch:
 * common_main/core/wmt_ic_soc.c:122-128,719-724,1079-1091,2042-2308. The
 * vendor loop starts the next patch on the same WMT/STP path: its init-script
 * helper is only a strict wmt_core_tx()/wmt_core_rx() exchange
 * (common_main/core/wmt_core.c:634-679), not a transport teardown or STP
 * renegotiation. The reset's firmware-internal side effects could not be
 * determined from this source. Therefore it is sent and matched only as the
 * vendor's ordinary full-STP transaction; any failure stops the manifest and
 * only the bootstrap session owner may fence/recover that transport.
 *
 * CFG_WMT_POWER_ON_DLM and CFG_WMT_PATCH_DL_OPTM are both hard-coded to 1 in
 * the cited vendor source. Their failures are fail-closed here; the bootstrap
 * session owner must fence/recover the full-STP window.
 *
 * This file is intentionally not a platform driver. It cannot acquire power,
 * clocks, resets, pads, BTIF DMA, STP ownership or a WMT endpoint. The only
 * entry consumes the canonical aggregate's already requested and hash-pinned
 * firmware bundle plus the bootstrap's opaque full-STP transaction lease. It
 * does not enable Wi-Fi, Bluetooth, GNSS, FM, RF, or any unknown rail.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_PATCH_DOWNLOAD_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/remoteproc/mtk_mt6797_consys.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-wmt-patch.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_CONSYS_WMT_PATCH_TIMEOUT_MS	3000
#define MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE	1000

#define MT6797_CONSYS_WMT_OPCODE_PATCH		0x01
#define MT6797_CONSYS_WMT_OPCODE_PATCH_ADDRESS	0x08
#define MT6797_CONSYS_WMT_OPCODE_MCU_CONFIG	0x08
#define MT6797_CONSYS_WMT_OPCODE_RESET		0x07

#define MT6797_CONSYS_WMT_PATCH_FRAGMENT_FIRST	0x01
#define MT6797_CONSYS_WMT_PATCH_FRAGMENT_MIDDLE	0x02
#define MT6797_CONSYS_WMT_PATCH_FRAGMENT_LAST	0x03

/* WMT_LIB_RX_TIMEOUT + 1000 is vendor MAX_EACH_WMT_CMD (wmt_lib.h:70,90). */
#define MT6797_CONSYS_WMT_PATCH_TIMEOUT \
	msecs_to_jiffies(MT6797_CONSYS_WMT_PATCH_TIMEOUT_MS)

/* Serialize this controller's manifests; the caller must exclusively own WMT. */
static DEFINE_MUTEX(mt6797_consys_wmt_patch_download_lock);

/* WMT_PATCH_ADDRESS_EVT and WMT_PATCH_EVT with the outer WMT header removed. */
static const u8 mt6797_consys_wmt_address_reply[] = { 0x00, 0x00, 0x00, 0x01 };
static const u8 mt6797_consys_wmt_patch_reply[] = { 0x00 };
static const u8 mt6797_consys_wmt_reset_data[] = { 0x04 };
static const u8 mt6797_consys_wmt_reset_reply[] = { 0x00 };
static const u8 mt6797_consys_wmt_mcu_config_reply[] = {
	0x00, 0x00, 0x00, 0x01,
};

static const struct mtk_wmt_match mt6797_consys_wmt_address_match = {
	.opcode = MT6797_CONSYS_WMT_OPCODE_PATCH_ADDRESS,
	.data = mt6797_consys_wmt_address_reply,
	.data_len = sizeof(mt6797_consys_wmt_address_reply),
};

static const struct mtk_wmt_match mt6797_consys_wmt_mcu_config_match = {
	.opcode = MT6797_CONSYS_WMT_OPCODE_MCU_CONFIG,
	.data = mt6797_consys_wmt_mcu_config_reply,
	.data_len = sizeof(mt6797_consys_wmt_mcu_config_reply),
};

static const struct mtk_wmt_match mt6797_consys_wmt_patch_match = {
	.opcode = MT6797_CONSYS_WMT_OPCODE_PATCH,
	.data = mt6797_consys_wmt_patch_reply,
	.data_len = sizeof(mt6797_consys_wmt_patch_reply),
};

static const struct mtk_wmt_match mt6797_consys_wmt_reset_match = {
	.opcode = MT6797_CONSYS_WMT_OPCODE_RESET,
	.data = mt6797_consys_wmt_reset_reply,
	.data_len = sizeof(mt6797_consys_wmt_reset_reply),
};

struct mt6797_consys_wmt_patch_transport {
	int (*transaction)(void *context, u8 opcode, const u8 *data,
			   size_t data_len, const struct mtk_wmt_match *expected,
			   unsigned long timeout);
	void *context;
};

/*
 * These are the operation-specific SDUs: mtk_wmt_transaction() prepends the
 * four WMT bytes (01 08 10 00). Values are the MT6797 0x0279 branch of the
 * vendor full and partial address commands at wmt_ic_soc.c:137-153,2163-2168.
 */
static const u8 mt6797_consys_wmt_full_address_data[] = {
	0x01, 0x01, 0x00, 0x01, 0x08, 0x05, 0x09, 0x02,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
};

static const u8 mt6797_consys_wmt_partial_address_template[] = {
	0x01, 0x01, 0x00, 0x01, 0x2c, 0x0b, 0x09, 0x02,
	0x00, 0x3f, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff,
};

/* The vendor's CFG_WMT_POWER_ON_DLM=1 envelope, SDUs after WMT's header. */
static const u8 mt6797_consys_wmt_power_on_dlm[][16] = {
	{
		0x01, 0x01, 0x00, 0x01, 0x60, 0x00, 0x10, 0x80,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x60, 0x00, 0x10, 0x80,
		0x00, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x60, 0x00, 0x10, 0x80,
		0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
	},
};

/* The vendor's CFG_WMT_PATCH_DL_OPTM=1 MT6797 clock-up envelope. */
static const u8 mt6797_consys_wmt_mcu_clock_up[][16] = {
	{
		0x01, 0x01, 0x00, 0x01, 0x10, 0x11, 0x02, 0x81,
		0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x0c, 0x01, 0x00, 0x80,
		0x40, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x18, 0x11, 0x02, 0x80,
		0x07, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x00, 0x11, 0x02, 0x81,
		0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	},
};

/* The vendor's CFG_WMT_PATCH_DL_OPTM=1 MT6797 clock-down envelope. */
static const u8 mt6797_consys_wmt_mcu_clock_down[][16] = {
	{
		0x01, 0x01, 0x00, 0x01, 0x00, 0x11, 0x02, 0x81,
		0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x0c, 0x01, 0x00, 0x80,
		0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
	},
	{
		0x01, 0x01, 0x00, 0x01, 0x10, 0x11, 0x02, 0x81,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
	},
};

static int mt6797_consys_wmt_patch_transaction(
		const struct mt6797_consys_wmt_patch_transport *transport,
		u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected)
{
	if (!transport || !transport->transaction)
		return -EINVAL;

	return transport->transaction(transport->context, opcode, data, data_len,
				      expected, MT6797_CONSYS_WMT_PATCH_TIMEOUT);
}

static int mt6797_consys_wmt_run_mcu_script(
		const struct mt6797_consys_wmt_patch_transport *transport,
		const u8 commands[][16], size_t command_count)
{
	size_t i;
	int ret;

	for (i = 0; i < command_count; i++) {
		ret = mt6797_consys_wmt_patch_transaction(transport,
				MT6797_CONSYS_WMT_OPCODE_MCU_CONFIG,
				commands[i], 16,
				&mt6797_consys_wmt_mcu_config_match);
		if (ret)
			return ret;
	}

	return 0;
}

static int
mt6797_consys_wmt_patch_set_address(
		const struct mt6797_consys_wmt_patch_transport *transport,
				    const u8 address[4])
{
	u8 partial_address[sizeof(mt6797_consys_wmt_partial_address_template)];
	int ret;

	if (!transport || !address)
		return -EINVAL;

	ret = mt6797_consys_wmt_patch_transaction(transport,
					    MT6797_CONSYS_WMT_OPCODE_PATCH_ADDRESS,
					    mt6797_consys_wmt_full_address_data,
					    sizeof(mt6797_consys_wmt_full_address_data),
					    &mt6797_consys_wmt_address_match);
	if (ret)
		return ret;

	memcpy(partial_address, mt6797_consys_wmt_partial_address_template,
	       sizeof(partial_address));
	/* Vendor full-command bytes 12--15 are SDU bytes 8--11. */
	memcpy(partial_address + 8, address, 4);
	return mt6797_consys_wmt_patch_transaction(transport,
		MT6797_CONSYS_WMT_OPCODE_PATCH_ADDRESS, partial_address,
		sizeof(partial_address), &mt6797_consys_wmt_address_match);
}

static int mt6797_consys_wmt_patch_send_body(
		const struct mt6797_consys_wmt_patch_transport *transport,
		const struct mt6797_consys_fw_patch_view *patch)
{
	u8 data[MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE + 1];
	const u8 *body;
	size_t offset = 0;
	size_t remaining;
	size_t fragment_size;
	int ret;

	if (!transport || !patch || !patch->payload || !patch->payload_size)
		return -EINVAL;

	body = patch->payload;
	remaining = patch->payload_size;
	while (remaining) {
		fragment_size = min_t(size_t, remaining,
				      MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE);
		if (fragment_size == remaining)
			data[0] = MT6797_CONSYS_WMT_PATCH_FRAGMENT_LAST;
		else if (!offset)
			data[0] = MT6797_CONSYS_WMT_PATCH_FRAGMENT_FIRST;
		else
			data[0] = MT6797_CONSYS_WMT_PATCH_FRAGMENT_MIDDLE;
		memcpy(data + 1, body + offset, fragment_size);

		ret = mt6797_consys_wmt_patch_transaction(transport,
						    MT6797_CONSYS_WMT_OPCODE_PATCH,
						    data, fragment_size + 1,
						    &mt6797_consys_wmt_patch_match);
		if (ret)
			return ret;

		offset += fragment_size;
		remaining -= fragment_size;
	}

	return 0;
}

static int
mt6797_consys_wmt_patch_download_views(const struct mt6797_consys_wmt_patch_transport *transport,
				       const struct mt6797_consys_fw_patch_view *patches)
{
	size_t i;
	int ret;

	if (!transport || !transport->transaction || !patches)
		return -EINVAL;
	for (i = 0; i < MT6797_CONSYS_FW_PATCH_COUNT; i++)
		if (!patches[i].payload || !patches[i].payload_size)
			return -EKEYREJECTED;

	/* Vendor CFG_WMT_POWER_ON_DLM=1, after full STP and before patch discovery. */
	ret = mt6797_consys_wmt_run_mcu_script(transport,
			mt6797_consys_wmt_power_on_dlm,
			ARRAY_SIZE(mt6797_consys_wmt_power_on_dlm));
	if (ret)
		return ret;

	/* Vendor CFG_WMT_PATCH_DL_OPTM=1, MT6797 (0x0279) clock-up table 3. */
	ret = mt6797_consys_wmt_run_mcu_script(transport,
			mt6797_consys_wmt_mcu_clock_up,
			ARRAY_SIZE(mt6797_consys_wmt_mcu_clock_up));
	if (ret)
		return ret;

	for (i = 0; i < MT6797_CONSYS_FW_PATCH_COUNT; i++) {
		ret = mt6797_consys_wmt_patch_set_address(transport, patches[i].address);
		if (ret)
			return ret;
		ret = mt6797_consys_wmt_patch_send_body(transport, &patches[i]);
		if (ret)
			return ret;
		ret = mt6797_consys_wmt_patch_transaction(transport,
					MT6797_CONSYS_WMT_OPCODE_RESET,
					mt6797_consys_wmt_reset_data,
					sizeof(mt6797_consys_wmt_reset_data),
					&mt6797_consys_wmt_reset_match);
		if (ret)
			return ret;
	}

	/* Vendor MT6797 clock-down table 4, only after every patch/reset. */
	return mt6797_consys_wmt_run_mcu_script(transport,
			mt6797_consys_wmt_mcu_clock_down,
			ARRAY_SIZE(mt6797_consys_wmt_mcu_clock_down));
}

static int mt6797_consys_wmt_patch_bootstrap_transaction(
		void *context, u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout)
{
	return mt6797_btif_stp_bootstrap_full_wmt_transaction(context, opcode, data,
							  data_len, expected, timeout);
}

int
mt6797_consys_wmt_patch_download(const struct mt6797_consys_fw_bundle *bundle,
				 const struct mt6797_btif_stp_bootstrap_full_wmt_lease *lease)
{
	struct mt6797_consys_fw_patch_view patches[MT6797_CONSYS_FW_PATCH_COUNT];
	struct mt6797_consys_wmt_patch_transport transport = {
		.transaction = mt6797_consys_wmt_patch_bootstrap_transaction,
		.context = (void *)lease,
	};
	unsigned int i;
	int ret;

	/*
	 * The caller must keep every other WMT producer quiesced for this entire
	 * call.  mtk_wmt_transaction() serializes individual packets, not this
	 * multi-packet manifest, and this local lock only excludes another caller
	 * of this controller.
	 */
	if (!bundle || !lease)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(patches); i++) {
		ret = mt6797_consys_fw_patch_view(bundle, i, &patches[i]);
		if (ret)
			return ret;
	}

	mutex_lock(&mt6797_consys_wmt_patch_download_lock);
	ret = mt6797_consys_wmt_patch_download_views(&transport, patches);
	/* The bootstrap owns recovery: this stage never calls mtk_wmt_reset(). */
	mutex_unlock(&mt6797_consys_wmt_patch_download_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_patch_download);

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_PATCH_DOWNLOAD_KUNIT_TEST)
/* Hardware-free packet-vector coverage for the static manifest helper. */

#define MT6797_CONSYS_WMT_PATCH_TEST_MAX_PACKETS	32
#define MT6797_CONSYS_WMT_PATCH_TEST_PACKET_SIZE \
	(MTK_WMT_HEADER_SIZE + MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE + 1)

struct mt6797_consys_wmt_patch_test_transport {
	struct mtk_stp stp;
	struct mtk_wmt *wmt;
	u8 packets[MT6797_CONSYS_WMT_PATCH_TEST_MAX_PACKETS]
		  [MT6797_CONSYS_WMT_PATCH_TEST_PACKET_SIZE];
	size_t packet_lengths[MT6797_CONSYS_WMT_PATCH_TEST_MAX_PACKETS];
	unsigned int packet_count;
	bool fail_transaction;
	unsigned int fail_packet;
};

static int
mt6797_consys_wmt_patch_test_receive(void *context, const u8 *payload,
				     size_t length)
{
	struct mt6797_consys_wmt_patch_test_transport *transport = context;
	static const u8 address_event[] = { 0x02, 0x08, 0x04, 0x00,
		0x00, 0x00, 0x00, 0x01 };
	static const u8 patch_event[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
	static const u8 reset_event[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
	unsigned int index = transport->packet_count;
	const u8 *reply;
	size_t reply_size;

	if (!payload || index >= MT6797_CONSYS_WMT_PATCH_TEST_MAX_PACKETS ||
	    length > MT6797_CONSYS_WMT_PATCH_TEST_PACKET_SIZE ||
	    length < MTK_WMT_HEADER_SIZE)
		return -EINVAL;

	memcpy(transport->packets[index], payload, length);
	transport->packet_lengths[index] = length;
	transport->packet_count++;
	if (transport->fail_transaction && index == transport->fail_packet)
		return -EIO;
	if (payload[1] == MT6797_CONSYS_WMT_OPCODE_PATCH_ADDRESS) {
		reply = address_event;
		reply_size = sizeof(address_event);
	} else if (payload[1] == MT6797_CONSYS_WMT_OPCODE_PATCH) {
		reply = patch_event;
		reply_size = sizeof(patch_event);
	} else if (payload[1] == MT6797_CONSYS_WMT_OPCODE_RESET) {
		reply = reset_event;
		reply_size = sizeof(reset_event);
	} else {
		return -EPROTO;
	}

	return mtk_wmt_receive(transport->wmt, reply, reply_size);
}

static int
mt6797_consys_wmt_patch_test_transmit(void *context, const u8 *frame,
				      size_t frame_length)
{
	struct mt6797_consys_wmt_patch_test_transport *transport = context;

	return mtk_stp_rx(&transport->stp, frame, frame_length);
}

static int mt6797_consys_wmt_patch_test_transaction(
		void *context, u8 opcode, const u8 *data, size_t data_len,
		const struct mtk_wmt_match *expected, unsigned long timeout)
{
	return mtk_wmt_transaction(context, opcode, data, data_len, expected,
				   timeout);
}

static void
mt6797_consys_wmt_patch_test_views(struct mt6797_consys_fw_patch_view
		patches[MT6797_CONSYS_FW_PATCH_COUNT],
		u8 *patch_data, size_t patch_size)
{
	patches[0] = (struct mt6797_consys_fw_patch_view) {
		.address = { 0x00, 0x00, 0x0a, 0xf0 },
		.payload = patch_data,
		.payload_size = patch_size,
	};
	patches[1] = patches[0];
	memcpy(patches[1].address, (u8[]){ 0x00, 0x00, 0x09, 0x00 }, 4);
}

static void mt6797_consys_wmt_patch_test_expect_mcu_script(
		struct kunit *test,
		struct mt6797_consys_wmt_patch_test_transport *transport,
		unsigned int first, const u8 commands[][16], unsigned int count)
{
	static const u8 header[] = { 0x01, 0x08, 0x10, 0x00 };
	unsigned int i;

	for (i = 0; i < count; i++) {
		KUNIT_EXPECT_EQ(test, transport->packet_lengths[first + i], 20U);
		KUNIT_EXPECT_MEMEQ(test, transport->packets[first + i], header,
				   sizeof(header));
		KUNIT_EXPECT_MEMEQ(test, transport->packets[first + i] + 4,
				   commands[i], 16);
	}
}

static void mt6797_consys_wmt_patch_test_manifest_sequence(struct kunit *test)
{
	static const u8 full_address[] = {
		0x01, 0x08, 0x10, 0x00,
		0x01, 0x01, 0x00, 0x01, 0x08, 0x05, 0x09, 0x02,
		0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const u8 first_partial_address[] = {
		0x01, 0x08, 0x10, 0x00,
		0x01, 0x01, 0x00, 0x01, 0x2c, 0x0b, 0x09, 0x02,
		0x00, 0x00, 0x0a, 0xf0, 0xff, 0xff, 0xff, 0xff,
	};
	static const u8 second_partial_address[] = {
		0x01, 0x08, 0x10, 0x00,
		0x01, 0x01, 0x00, 0x01, 0x2c, 0x0b, 0x09, 0x02,
		0x00, 0x00, 0x09, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const u8 patch_last[] = { 0x01, 0x01, 0x02, 0x00, 0x03, 'X' };
	static const u8 reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	struct mt6797_consys_wmt_patch_test_transport *transport;
	struct mt6797_consys_wmt_patch_transport patch_transport;
	struct mt6797_consys_fw_patch_view patches[MT6797_CONSYS_FW_PATCH_COUNT];
	struct mtk_wmt wmt;
	u8 patch_data[] = { 'X' };
	int ret;

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	mtk_wmt_init(&wmt, MTK_STP_MODE_FULL,
		     mt6797_consys_wmt_patch_test_transmit, transport);
	transport->wmt = &wmt;
	mtk_stp_init(&transport->stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(&transport->stp, MTK_STP_WMT,
						      mt6797_consys_wmt_patch_test_receive,
						      transport), 0);
	mt6797_consys_wmt_patch_test_views(patches, patch_data, sizeof(patch_data));
	patch_transport = (struct mt6797_consys_wmt_patch_transport) {
		.transaction = mt6797_consys_wmt_patch_test_transaction,
		.context = &wmt,
	};

	ret = mt6797_consys_wmt_patch_download_views(&patch_transport, patches);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, transport->packet_count, 18U);
	mt6797_consys_wmt_patch_test_expect_mcu_script(
		test, transport, 0, mt6797_consys_wmt_power_on_dlm,
		ARRAY_SIZE(mt6797_consys_wmt_power_on_dlm));
	mt6797_consys_wmt_patch_test_expect_mcu_script(
		test, transport, 3, mt6797_consys_wmt_mcu_clock_up,
		ARRAY_SIZE(mt6797_consys_wmt_mcu_clock_up));
	mt6797_consys_wmt_patch_test_expect_mcu_script(
		test, transport, 15, mt6797_consys_wmt_mcu_clock_down,
		ARRAY_SIZE(mt6797_consys_wmt_mcu_clock_down));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[7], sizeof(full_address));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[8],
				sizeof(first_partial_address));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[9], sizeof(patch_last));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[10], sizeof(reset));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[11], sizeof(full_address));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[12],
				sizeof(second_partial_address));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[13], sizeof(patch_last));
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[14], sizeof(reset));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[7], full_address,
			   sizeof(full_address));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[8], first_partial_address,
			   sizeof(first_partial_address));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[9], patch_last,
			   sizeof(patch_last));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[10], reset, sizeof(reset));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[11], full_address,
			   sizeof(full_address));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[12], second_partial_address,
			   sizeof(second_partial_address));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[13], patch_last,
			   sizeof(patch_last));
	KUNIT_EXPECT_MEMEQ(test, transport->packets[14], reset, sizeof(reset));
}

static void mt6797_consys_wmt_patch_test_envelope_failure_fences(
		struct kunit *test)
{
	struct mt6797_consys_wmt_patch_test_transport *transport;
	struct mt6797_consys_wmt_patch_transport patch_transport;
	struct mt6797_consys_fw_patch_view patches[MT6797_CONSYS_FW_PATCH_COUNT];
	struct mtk_wmt wmt;
	u8 patch_data[] = { 'X' };
	int ret;

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	mtk_wmt_init(&wmt, MTK_STP_MODE_FULL,
			 mt6797_consys_wmt_patch_test_transmit, transport);
	transport->wmt = &wmt;
	transport->fail_transaction = true;
	transport->fail_packet = 2;
	mtk_stp_init(&transport->stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(&transport->stp, MTK_STP_WMT,
							      mt6797_consys_wmt_patch_test_receive,
							      transport), 0);
	mt6797_consys_wmt_patch_test_views(patches, patch_data, sizeof(patch_data));
	patch_transport = (struct mt6797_consys_wmt_patch_transport) {
		.transaction = mt6797_consys_wmt_patch_test_transaction,
		.context = &wmt,
	};

	ret = mt6797_consys_wmt_patch_download_views(&patch_transport, patches);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, transport->packet_count, 3U);
	transport->fail_transaction = false;
	ret = mt6797_consys_wmt_patch_download_views(&patch_transport, patches);
	KUNIT_EXPECT_EQ(test, ret, -EPIPE);
	KUNIT_EXPECT_EQ(test, transport->packet_count, 3U);
}

static void mt6797_consys_wmt_patch_test_fragment_tags(struct kunit *test)
{
	struct mt6797_consys_wmt_patch_test_transport *transport;
	struct mt6797_consys_wmt_patch_transport patch_transport;
	struct mt6797_consys_fw_patch_view patch = { .payload_size = 2001 };
	struct mtk_wmt wmt;
	u8 *patch_data;

	patch_data = kunit_kzalloc(test, patch.payload_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, patch_data);
	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	patch.payload = patch_data;
	mtk_wmt_init(&wmt, MTK_STP_MODE_FULL,
		     mt6797_consys_wmt_patch_test_transmit, transport);
	transport->wmt = &wmt;
	mtk_stp_init(&transport->stp, MTK_STP_MODE_FULL);
	KUNIT_ASSERT_EQ(test, mtk_stp_register_client(&transport->stp, MTK_STP_WMT,
						      mt6797_consys_wmt_patch_test_receive,
						      transport), 0);
	patch_transport = (struct mt6797_consys_wmt_patch_transport) {
		.transaction = mt6797_consys_wmt_patch_test_transaction,
		.context = &wmt,
	};

	KUNIT_ASSERT_EQ(test,
			mt6797_consys_wmt_patch_send_body(&patch_transport, &patch), 0);
	KUNIT_ASSERT_EQ(test, transport->packet_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->packets[0][4],
			MT6797_CONSYS_WMT_PATCH_FRAGMENT_FIRST);
	KUNIT_EXPECT_EQ(test, transport->packets[1][4],
			MT6797_CONSYS_WMT_PATCH_FRAGMENT_MIDDLE);
	KUNIT_EXPECT_EQ(test, transport->packets[2][4],
			MT6797_CONSYS_WMT_PATCH_FRAGMENT_LAST);
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[0],
			(size_t)MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE + 5);
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[1],
			(size_t)MT6797_CONSYS_WMT_PATCH_FRAGMENT_SIZE + 5);
	KUNIT_EXPECT_EQ(test, transport->packet_lengths[2], (size_t)6);
}

static void mt6797_consys_wmt_patch_test_unowned_transport_rejected(struct kunit *test)
{
	u8 patch_data[] = { 'X' };
	struct mt6797_consys_fw_patch_view patches[MT6797_CONSYS_FW_PATCH_COUNT];
	int ret;

	mt6797_consys_wmt_patch_test_views(patches, patch_data, sizeof(patch_data));

	/* Only bootstrap can create a production full-WMT transport capability. */
	ret = mt6797_consys_wmt_patch_download_views(NULL, patches);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case mt6797_consys_wmt_patch_test_cases[] = {
	KUNIT_CASE(mt6797_consys_wmt_patch_test_manifest_sequence),
	KUNIT_CASE(mt6797_consys_wmt_patch_test_envelope_failure_fences),
	KUNIT_CASE(mt6797_consys_wmt_patch_test_fragment_tags),
	KUNIT_CASE(mt6797_consys_wmt_patch_test_unowned_transport_rejected),
	{ }
};

static struct kunit_suite mt6797_consys_wmt_patch_test_suite = {
	.name = "mt6797-consys-wmt-patch",
	.test_cases = mt6797_consys_wmt_patch_test_cases,
};

kunit_test_suite(mt6797_consys_wmt_patch_test_suite);
#endif

MODULE_DESCRIPTION("Bounded MT6797 WMT patch-download controller");
MODULE_LICENSE("GPL");
