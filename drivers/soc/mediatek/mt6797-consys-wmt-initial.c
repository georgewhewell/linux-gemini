// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dormant MT6797 mandatory-STP identity/default-query controller.
 *
 * Vendor wmt_core.c:202-228 declares the raw register packet/event layout;
 * :585-631 fills its read opcode/address/value/mask fields and validates the
 * returned address/value.  :909-1002 reads GEN_HCR (0x80000008, mask
 * 0x0000ffff), selects SOC operations, then calls the selected ic_ver_check
 * at :992-998.  The SOC version check in wmt_ic_soc.c:1614-1659 reads HVR
 * (0x80000000) and FVR (0x80000004), each masked by 0x0000ffff, before
 * selecting an exact supported HVR table entry at :875-905.  The vendor
 * accidentally ignores the FVR transaction return at :1633-1636; this
 * controller does not reproduce that bug.
 * The vendor raw event is 16 bytes.  Its checked-in static source template
 * claims a length byte of 0x04, but the working Gemini vendor capture records
 * the complete, self-consistent wire header as 02 08 0c 00 followed by the
 * eight-byte register result (bsg100 H35, 105.258714--105.258966).  The
 * latter is the only representation compatible with the actual 16-byte
 * message and is the contract used here.  It still cannot be handed to the
 * generic strict WMT event decoder because this bootstrap owns the raw
 * register exchange itself.
 * This controller instead accepts only the complete vendor layout below.
 *
 * Only after this complete identity preflight, wmt_ic_soc.c:122-124 defines
 * the first BTIF mandatory-STP scripted query/default reply.  Its
 * :190-191 set-full command/reply follows that query at :1008-1022, before
 * the host changes its own STP framing at :1024-1037.  This controller owns
 * only the exact mandatory-mode query and the chip-side set command.  It
 * never changes the local host framing, waits for the chip, or verifies the
 * full-mode reply.
 *
 * There is no DT match, MMIO, clock, reset, firmware, patch, or radio action.
 * Its transport owner supplies the already-live mandatory-STP WMT endpoint
 * and one raw exchange callback.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_INITIAL_KUNIT_TEST)
#include <kunit/test.h>
#endif

#include <linux/soc/mediatek/mt6797-consys-wmt-initial.h>
#include <linux/soc/mediatek/mtk-stp.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_CONSYS_WMT_GEN_HVR		0x80000000
#define MT6797_CONSYS_WMT_GEN_FVR		0x80000004
#define MT6797_CONSYS_WMT_GEN_HCR		0x80000008
#define MT6797_CONSYS_WMT_GEN_VER_MASK	0x0000ffff
#define MT6797_CONSYS_WMT_GEN_HCR_MASK	0x0000ffff
#define MT6797_CONSYS_WMT_CHIP_ID		0x0279

#define MT6797_CONSYS_WMT_HVR_E1		0x8a00
#define MT6797_CONSYS_WMT_HVR_E2		0x8a01
#define MT6797_CONSYS_WMT_HVR_E3		0x8b01

#define MT6797_CONSYS_WMT_REG_OPCODE		0x08
#define MT6797_CONSYS_WMT_REG_COMMAND_SIZE	20
#define MT6797_CONSYS_WMT_REG_EVENT_SIZE	16

#define MT6797_CONSYS_WMT_STP_OPCODE		0x04

enum mt6797_consys_wmt_initial_state {
	MT6797_CONSYS_WMT_INITIAL_NEW,
	MT6797_CONSYS_WMT_INITIAL_RUNNING,
	MT6797_CONSYS_WMT_INITIAL_READY,
	MT6797_CONSYS_WMT_INITIAL_SETTING_CHIP_FULL,
	MT6797_CONSYS_WMT_INITIAL_CHIP_FULL_CONFIGURED,
	MT6797_CONSYS_WMT_INITIAL_FAILED,
};

struct mt6797_consys_wmt_initial {
	/* Serializes the one run and its terminal state transition. */
	struct mutex lock;
	struct mtk_wmt *wmt;
	struct mt6797_consys_wmt_initial_ops ops;
	enum mt6797_consys_wmt_initial_state state;
};

/*
 * wmt_core.c:202-210 then :592-595.  HCR/HVR/FVR address and mask fields are
 * rendered explicitly as little-endian bytes rather than copying host-endian
 * values into a mutable vendor buffer.
 */
static const u8 mt6797_consys_wmt_reg_read_template[] = {
	0x01, MT6797_CONSYS_WMT_REG_OPCODE, 0x10, 0x00,
	0x02, 0x01, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0x00, 0x00,
};

/*
 * The vendor source's WMT_SET_REG_RD_EVT initializer has 0x04 here, but the
 * live vendor DMA trace proves 0x0c: 12 bytes follow the WMT header (status,
 * register descriptor, address, value).  Do not copy that stale initializer
 * into a parser which otherwise requires all sixteen bytes.
 */
static const u8 mt6797_consys_wmt_reg_event_prefix[] = {
	0x02, MT6797_CONSYS_WMT_REG_OPCODE, 0x0c, 0x00,
	0x00, 0x00, 0x00, 0x01,
};

/* wmt_ic_soc.c:122-124, with the outer WMT header removed for mtk_wmt. */
static const u8 mt6797_consys_wmt_default_query_data[] = { 0x04 };
static const u8 mt6797_consys_wmt_default_query_event[] = {
	0x00, 0x04, 0x11, 0x00, 0x00, 0x00,
};

static const struct mtk_wmt_match mt6797_consys_wmt_default_query_match = {
	.opcode = MT6797_CONSYS_WMT_STP_OPCODE,
	.data = mt6797_consys_wmt_default_query_event,
	.data_len = sizeof(mt6797_consys_wmt_default_query_event),
};

/* wmt_ic_soc.c:190-191, with the outer WMT header removed for mtk_wmt. */
static const u8 mt6797_consys_wmt_set_chip_full_data[] = {
	0x03, 0xdf, 0x0e, 0x68, 0x01,
};

static const u8 mt6797_consys_wmt_set_chip_full_event[] = { 0x00, 0x03 };

static const struct mtk_wmt_match mt6797_consys_wmt_set_chip_full_match = {
	.opcode = MT6797_CONSYS_WMT_STP_OPCODE,
	.data = mt6797_consys_wmt_set_chip_full_event,
	.data_len = sizeof(mt6797_consys_wmt_set_chip_full_event),
};

/* wmt_ic_soc.c:875-905, exact entries only; no vendor major-version fallback. */
static bool mt6797_consys_wmt_hvr_supported(u16 hardware_version)
{
	switch (hardware_version) {
	case MT6797_CONSYS_WMT_HVR_E1:
	case MT6797_CONSYS_WMT_HVR_E2:
	case MT6797_CONSYS_WMT_HVR_E3:
		return true;
	default:
		return false;
	}
}

static int
mt6797_consys_wmt_reg_read(struct mt6797_consys_wmt_initial *initial,
			   u32 expected_address, u32 mask, unsigned long timeout,
			   u16 *value_out)
{
	u8 command[MT6797_CONSYS_WMT_REG_COMMAND_SIZE];
	u8 event[MT6797_CONSYS_WMT_REG_EVENT_SIZE];
	size_t event_len = 0;
	u32 address;
	u32 value;
	int ret;

	memcpy(command, mt6797_consys_wmt_reg_read_template, sizeof(command));
	put_unaligned_le32(expected_address, command + 8);
	put_unaligned_le32(mask, command + 16);
	ret = initial->ops.raw_exchange(initial->ops.context,
					command, sizeof(command),
					event, sizeof(event), &event_len, timeout);
	if (ret)
		return ret;
	if (event_len != sizeof(event))
		return -EMSGSIZE;
	if (memcmp(event, mt6797_consys_wmt_reg_event_prefix,
		   sizeof(mt6797_consys_wmt_reg_event_prefix)))
		return -EPROTO;

	address = get_unaligned_le32(event + 8);
	value = get_unaligned_le32(event + 12);
	if (address != expected_address)
		return -EPROTO;

	*value_out = value & mask;
	return 0;
}

static int mt6797_wmt_initial_run_unchecked(struct mt6797_consys_wmt_initial *initial,
					    unsigned long timeout,
					    struct mt6797_consys_wmt_initial_identity *identity)
{
	int ret;

	if (!initial || !identity || !timeout)
		return -EINVAL;
	memset(identity, 0, sizeof(*identity));

	mutex_lock(&initial->lock);
	if (initial->state == MT6797_CONSYS_WMT_INITIAL_FAILED) {
		ret = -EPIPE;
	} else if (initial->state == MT6797_CONSYS_WMT_INITIAL_READY ||
		   initial->state == MT6797_CONSYS_WMT_INITIAL_CHIP_FULL_CONFIGURED) {
		ret = -EALREADY;
	} else if (initial->state != MT6797_CONSYS_WMT_INITIAL_NEW) {
		ret = -EBUSY;
	} else if (initial->wmt->stp_mode != MTK_STP_MODE_MANDATORY) {
		initial->state = MT6797_CONSYS_WMT_INITIAL_FAILED;
		ret = -EPROTONOSUPPORT;
	} else {
		initial->state = MT6797_CONSYS_WMT_INITIAL_RUNNING;
		ret = 0;
	}
	mutex_unlock(&initial->lock);
	if (ret)
		return ret;

	ret = mt6797_consys_wmt_reg_read(initial, MT6797_CONSYS_WMT_GEN_HCR,
					 MT6797_CONSYS_WMT_GEN_HCR_MASK, timeout,
					 &identity->chip_id);
	if (!ret && identity->chip_id != MT6797_CONSYS_WMT_CHIP_ID)
		ret = -ENODEV;
	if (!ret)
		ret = mt6797_consys_wmt_reg_read(initial, MT6797_CONSYS_WMT_GEN_HVR,
						 MT6797_CONSYS_WMT_GEN_VER_MASK, timeout,
						 &identity->hardware_version);
	if (!ret && !mt6797_consys_wmt_hvr_supported(identity->hardware_version))
		ret = -ENODEV;
	if (!ret)
		ret = mt6797_consys_wmt_reg_read(initial, MT6797_CONSYS_WMT_GEN_FVR,
						 MT6797_CONSYS_WMT_GEN_VER_MASK, timeout,
						 &identity->firmware_version);
	if (!ret)
		ret = mtk_wmt_transaction(initial->wmt,
					  MT6797_CONSYS_WMT_STP_OPCODE,
					  mt6797_consys_wmt_default_query_data,
					  sizeof(mt6797_consys_wmt_default_query_data),
					  &mt6797_consys_wmt_default_query_match,
					  timeout);

	mutex_lock(&initial->lock);
	initial->state = ret ? MT6797_CONSYS_WMT_INITIAL_FAILED :
		MT6797_CONSYS_WMT_INITIAL_READY;
	mutex_unlock(&initial->lock);
	return ret;
}

/*
 * wmt_ic_soc.c:1008-1022 issues the default query and then this exact
 * chip-side command while the host remains in mandatory STP mode.  The PIO
 * bootstrap controller owns the later host/full-session transition, so this
 * helper must never select MTK_STP_MODE_FULL or transmit its full-mode query.
 */
static int
mt6797_wmt_initial_set_full_unchecked(struct mt6797_consys_wmt_initial *initial,
				      unsigned long timeout)
{
	int ret;

	if (!initial || !timeout)
		return -EINVAL;

	mutex_lock(&initial->lock);
	if (initial->state == MT6797_CONSYS_WMT_INITIAL_FAILED) {
		ret = -EPIPE;
	} else if (initial->state ==
		   MT6797_CONSYS_WMT_INITIAL_CHIP_FULL_CONFIGURED) {
		ret = -EALREADY;
	} else if (initial->state == MT6797_CONSYS_WMT_INITIAL_RUNNING ||
		   initial->state == MT6797_CONSYS_WMT_INITIAL_SETTING_CHIP_FULL) {
		ret = -EBUSY;
	} else if (initial->state != MT6797_CONSYS_WMT_INITIAL_READY) {
		/* No command was sent, but this owner order is unsafe to continue. */
		initial->state = MT6797_CONSYS_WMT_INITIAL_FAILED;
		ret = -EPROTO;
	} else if (initial->wmt->stp_mode != MTK_STP_MODE_MANDATORY) {
		initial->state = MT6797_CONSYS_WMT_INITIAL_FAILED;
		ret = -EPROTONOSUPPORT;
	} else {
		initial->state = MT6797_CONSYS_WMT_INITIAL_SETTING_CHIP_FULL;
		ret = 0;
	}
	mutex_unlock(&initial->lock);
	if (ret)
		return ret;

	ret = mtk_wmt_transaction(initial->wmt, MT6797_CONSYS_WMT_STP_OPCODE,
				  mt6797_consys_wmt_set_chip_full_data,
				  sizeof(mt6797_consys_wmt_set_chip_full_data),
				  &mt6797_consys_wmt_set_chip_full_match, timeout);

	mutex_lock(&initial->lock);
	initial->state = ret ? MT6797_CONSYS_WMT_INITIAL_FAILED :
		MT6797_CONSYS_WMT_INITIAL_CHIP_FULL_CONFIGURED;
	mutex_unlock(&initial->lock);
	return ret;
}

struct mt6797_consys_wmt_initial *
mt6797_consys_wmt_initial_create(struct mtk_wmt *wmt,
				 const struct mt6797_consys_wmt_initial_ops *ops)
{
	struct mt6797_consys_wmt_initial *initial;

	if (!wmt || !ops || !ops->raw_exchange)
		return ERR_PTR(-EINVAL);

	initial = kzalloc_obj(*initial, GFP_KERNEL);
	if (!initial)
		return ERR_PTR(-ENOMEM);
	mutex_init(&initial->lock);
	initial->wmt = wmt;
	initial->ops = *ops;
	return initial;
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_initial_create);

void mt6797_consys_wmt_initial_destroy(struct mt6797_consys_wmt_initial *initial)
{
	if (!initial || IS_ERR(initial))
		return;

	/* The single transport owner serializes destroy against run(). */
	kfree(initial);
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_initial_destroy);

int mt6797_consys_wmt_initial_run(struct mt6797_consys_wmt_initial *initial,
					struct mt6797_consys_wmt_initial_identity *identity,
					unsigned long timeout)
{
	return mt6797_wmt_initial_run_unchecked(initial, timeout, identity);
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_initial_run);

int
mt6797_consys_wmt_initial_configure_chip_full(
		struct mt6797_consys_wmt_initial *initial, unsigned long timeout)
{
	return mt6797_wmt_initial_set_full_unchecked(initial, timeout);
}
EXPORT_SYMBOL_GPL(mt6797_consys_wmt_initial_configure_chip_full);

#if IS_ENABLED(CONFIG_MTK_MT6797_CONSYS_WMT_INITIAL_KUNIT_TEST)

enum mt6797_consys_wmt_initial_test_reply {
	MT6797_CONSYS_WMT_INITIAL_TEST_SUCCESS,
	MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_ERROR,
	MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_MALFORMED,
	MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HVR_ERROR,
	MT6797_CONSYS_WMT_INITIAL_TEST_RAW_FVR_ERROR,
	MT6797_CONSYS_WMT_INITIAL_TEST_BAD_CHIP,
	MT6797_CONSYS_WMT_INITIAL_TEST_BAD_HVR,
	MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_MALFORMED,
	MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_UNEXPECTED,
	MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_TIMEOUT,
	MT6797_CONSYS_WMT_INITIAL_TEST_SET_MALFORMED,
	MT6797_CONSYS_WMT_INITIAL_TEST_SET_UNEXPECTED,
	MT6797_CONSYS_WMT_INITIAL_TEST_SET_TIMEOUT,
};

struct mt6797_consys_wmt_initial_test_transport {
	struct kunit *test;
	struct mtk_wmt wmt;
	enum mt6797_consys_wmt_initial_test_reply reply;
	u8 raw_commands[3][MT6797_CONSYS_WMT_REG_COMMAND_SIZE];
	size_t raw_command_lens[3];
	u8 raw_events[3][MT6797_CONSYS_WMT_REG_EVENT_SIZE];
	size_t raw_event_lens[3];
	u8 wmt_commands[2][MTK_WMT_HEADER_SIZE +
			   sizeof(mt6797_consys_wmt_set_chip_full_data)];
	size_t wmt_command_lens[2];
	unsigned int steps;
	unsigned int raw_count;
	unsigned int wmt_count;
	unsigned int default_query_count;
	unsigned int set_count;
};

static void
mt6797_consys_wmt_initial_test_reg_event(u8 *event, u32 address, u32 value)
{
	memcpy(event, mt6797_consys_wmt_reg_event_prefix,
	       sizeof(mt6797_consys_wmt_reg_event_prefix));
	put_unaligned_le32(address, event + 8);
	put_unaligned_le32(value, event + 12);
}

static int mt6797_wmt_initial_test_raw_exchange(void *context,
						const u8 *command,
						  size_t command_len, u8 *event,
						  size_t event_capacity, size_t *event_len,
						  unsigned long timeout)
{
	struct mt6797_consys_wmt_initial_test_transport *transport = context;
	unsigned int index = transport->raw_count;
	u32 address;
	u32 value;

	if (index >= ARRAY_SIZE(transport->raw_commands))
		return -EOVERFLOW;
	KUNIT_EXPECT_EQ(transport->test, transport->steps, index);
	KUNIT_EXPECT_EQ(transport->test, command_len,
			sizeof(mt6797_consys_wmt_reg_read_template));
	memcpy(transport->raw_commands[index], command, command_len);
	transport->raw_command_lens[index] = command_len;
	transport->raw_count++;
	transport->steps++;
	if ((index == 0 &&
	     transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_ERROR) ||
	    (index == 1 &&
	     transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HVR_ERROR) ||
	    (index == 2 &&
	     transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_RAW_FVR_ERROR))
		return -ETIMEDOUT;
	if (event_capacity < MT6797_CONSYS_WMT_REG_EVENT_SIZE)
		return -ENOSPC;

	address = get_unaligned_le32(command + 8);
	switch (index) {
	case 0:
		value = MT6797_CONSYS_WMT_CHIP_ID;
		if (transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_BAD_CHIP)
			value = 0x0326;
		break;
	case 1:
		/* Gemini vendor H35 capture: HVR read returned 0x00008a00. */
		value = MT6797_CONSYS_WMT_HVR_E1;
		if (transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_BAD_HVR)
			value = 0xa5a58c00;
		break;
	case 2:
		/* Gemini vendor H35 capture: FVR read returned 0x00008a00. */
		value = MT6797_CONSYS_WMT_HVR_E1;
		break;
	default:
		return -EPROTO;
	}
	mt6797_consys_wmt_initial_test_reg_event(event, address, value);
	*event_len = MT6797_CONSYS_WMT_REG_EVENT_SIZE;
	memcpy(transport->raw_events[index], event, *event_len);
	transport->raw_event_lens[index] = *event_len;
	if (transport->reply == MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_MALFORMED)
		event[8] ^= 1;
	return 0;
}

static int mt6797_wmt_initial_test_query_transmit(void *context,
						  const u8 *packet, size_t packet_len)
{
	struct mt6797_consys_wmt_initial_test_transport *transport = context;
	static const u8 default_event[] = {
		0x02, MT6797_CONSYS_WMT_STP_OPCODE, 0x06, 0x00,
		0x00, 0x04, 0x11, 0x00, 0x00, 0x00,
	};
	static const u8 set_event[] = {
		0x02, MT6797_CONSYS_WMT_STP_OPCODE, 0x02, 0x00, 0x00, 0x03,
	};
	const u8 *event;
	size_t event_len;
	enum mt6797_consys_wmt_initial_test_reply malformed;
	enum mt6797_consys_wmt_initial_test_reply unexpected;
	enum mt6797_consys_wmt_initial_test_reply timed_out;
	unsigned int index = transport->wmt_count;
	u8 malformed_event[sizeof(default_event) - 1];
	u8 unexpected_event[sizeof(default_event)];

	if (index >= ARRAY_SIZE(transport->wmt_commands))
		return -EOVERFLOW;
	KUNIT_EXPECT_EQ(transport->test, transport->steps, index + 3);
	transport->steps++;
	KUNIT_EXPECT_LE(transport->test, packet_len,
			sizeof(transport->wmt_commands[index]));
	memcpy(transport->wmt_commands[index], packet, packet_len);
	transport->wmt_command_lens[index] = packet_len;
	transport->wmt_count++;
	if (index == 0) {
		transport->default_query_count++;
		event = default_event;
		event_len = sizeof(default_event);
		malformed = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_MALFORMED;
		unexpected = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_UNEXPECTED;
		timed_out = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_TIMEOUT;
	} else {
		transport->set_count++;
		event = set_event;
		event_len = sizeof(set_event);
		malformed = MT6797_CONSYS_WMT_INITIAL_TEST_SET_MALFORMED;
		unexpected = MT6797_CONSYS_WMT_INITIAL_TEST_SET_UNEXPECTED;
		timed_out = MT6797_CONSYS_WMT_INITIAL_TEST_SET_TIMEOUT;
	}
	if (transport->reply == timed_out)
		return 0;
	if (transport->reply == malformed) {
		if (event_len - 1 > sizeof(malformed_event))
			return -EOVERFLOW;
		memcpy(malformed_event, event, event_len - 1);
		return mtk_wmt_receive(&transport->wmt, malformed_event,
					       event_len - 1);
	}
	if (transport->reply == unexpected) {
		if (event_len > sizeof(unexpected_event))
			return -EOVERFLOW;
		memcpy(unexpected_event, event, event_len);
		unexpected_event[event_len - 1] ^= 1;
		return mtk_wmt_receive(&transport->wmt, unexpected_event,
					       event_len);
	}
	return mtk_wmt_receive(&transport->wmt, event, event_len);
}

static struct mt6797_consys_wmt_initial *
mt6797_wmt_initial_test_create(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial_ops ops;
	struct mt6797_consys_wmt_initial *initial;

	transport = kunit_kzalloc(test, sizeof(*transport), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, transport);
	transport->test = test;
	mtk_wmt_init_payload_transport(&transport->wmt, MTK_STP_MODE_MANDATORY,
				       mt6797_wmt_initial_test_query_transmit,
				       transport);
	ops = (struct mt6797_consys_wmt_initial_ops) {
		.raw_exchange = mt6797_wmt_initial_test_raw_exchange,
		.context = transport,
	};
	initial = mt6797_consys_wmt_initial_create(&transport->wmt, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(initial));
	test->priv = transport;
	return initial;
}

static void mt6797_consys_wmt_initial_test_sequence(struct kunit *test)
{
	static const u8 raw_commands[][MT6797_CONSYS_WMT_REG_COMMAND_SIZE] = {
		{
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		}, {
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		}, {
			0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
			0x04, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0x00, 0x00,
		},
	};
	static const u8 raw_events[][MT6797_CONSYS_WMT_REG_EVENT_SIZE] = {
		{
			0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x08, 0x00, 0x00, 0x80, 0x79, 0x02, 0x00, 0x00,
		}, {
			0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x80, 0x00, 0x8a, 0x00, 0x00,
		}, {
			0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x04, 0x00, 0x00, 0x80, 0x00, 0x8a, 0x00, 0x00,
		},
	};
	static const u8 query_command[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;
	int ret;
	size_t i;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	ret = mt6797_wmt_initial_run_unchecked(initial, msecs_to_jiffies(100),
					       &identity);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, identity.chip_id, (u16)MT6797_CONSYS_WMT_CHIP_ID);
	KUNIT_EXPECT_EQ(test, identity.hardware_version,
			(u16)MT6797_CONSYS_WMT_HVR_E1);
	KUNIT_EXPECT_EQ(test, identity.firmware_version,
			(u16)MT6797_CONSYS_WMT_HVR_E1);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
	KUNIT_EXPECT_EQ(test, transport->set_count, 0U);
	for (i = 0; i < ARRAY_SIZE(raw_commands); i++) {
		KUNIT_EXPECT_EQ(test, transport->raw_command_lens[i],
				MT6797_CONSYS_WMT_REG_COMMAND_SIZE);
		KUNIT_EXPECT_MEMEQ(test, transport->raw_commands[i], raw_commands[i],
				   sizeof(raw_commands[i]));
		KUNIT_EXPECT_EQ(test, transport->raw_event_lens[i],
				MT6797_CONSYS_WMT_REG_EVENT_SIZE);
		KUNIT_EXPECT_MEMEQ(test, transport->raw_events[i], raw_events[i],
				   sizeof(raw_events[i]));
	}
	KUNIT_EXPECT_MEMEQ(test, transport->wmt_commands[0], query_command,
			   sizeof(query_command));
	KUNIT_EXPECT_EQ(test, transport->wmt_command_lens[0],
			MTK_WMT_HEADER_SIZE + sizeof(mt6797_consys_wmt_default_query_data));
	ret = mt6797_wmt_initial_run_unchecked(initial, msecs_to_jiffies(100),
					       &identity);
	KUNIT_EXPECT_EQ(test, ret, -EALREADY);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_raw_failure_fails(struct kunit *test)
{
	static const struct {
		enum mt6797_consys_wmt_initial_test_reply reply;
		unsigned int raw_count;
	} failures[] = {
		{ MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_ERROR, 1 },
		{ MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HVR_ERROR, 2 },
		{ MT6797_CONSYS_WMT_INITIAL_TEST_RAW_FVR_ERROR, 3 },
	};
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		initial = mt6797_wmt_initial_test_create(test);
		transport = test->priv;
		transport->reply = failures[i].reply;
		KUNIT_EXPECT_EQ(test,
				mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-ETIMEDOUT);
		KUNIT_EXPECT_EQ(test, transport->raw_count, failures[i].raw_count);
		KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
		KUNIT_EXPECT_EQ(test,
				mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
		mt6797_consys_wmt_initial_destroy(initial);
	}

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_RAW_HCR_MALFORMED;
	KUNIT_EXPECT_EQ(test,
			mt6797_wmt_initial_run_unchecked(initial, 1, &identity), -EPROTO);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 1U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_chip_rejection(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_BAD_CHIP;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-ENODEV);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 1U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_hvr_rejection(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_BAD_HVR;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-ENODEV);
	KUNIT_EXPECT_EQ(test, identity.hardware_version, (u16)0x8c00);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 2U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_query_errors_fail(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_MALFORMED;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_UNEXPECTED;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->reply = MT6797_CONSYS_WMT_INITIAL_TEST_QUERY_TIMEOUT;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_chip_full_sequence(struct kunit *test)
{
	static const u8 set_command[] = {
		0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01,
	};
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;
	int ret;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	KUNIT_ASSERT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			0);
	ret = mt6797_wmt_initial_set_full_unchecked(initial, 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
	KUNIT_EXPECT_EQ(test, transport->set_count, 1U);
	KUNIT_EXPECT_EQ(test, transport->wmt_count, 2U);
	KUNIT_EXPECT_EQ(test, transport->wmt_command_lens[1],
			MTK_WMT_HEADER_SIZE + sizeof(mt6797_consys_wmt_set_chip_full_data));
	KUNIT_EXPECT_MEMEQ(test, transport->wmt_commands[1], set_command,
			   sizeof(set_command));
	KUNIT_EXPECT_EQ(test,
			mt6797_wmt_initial_set_full_unchecked(initial, 1),
			-EALREADY);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_chip_full_requires_ready(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	KUNIT_EXPECT_EQ(test,
			mt6797_wmt_initial_set_full_unchecked(initial, 1),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 0U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
	KUNIT_EXPECT_EQ(test, transport->set_count, 0U);
	KUNIT_EXPECT_EQ(test,
			mt6797_wmt_initial_set_full_unchecked(initial, 1),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	KUNIT_ASSERT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			0);
	transport->wmt.stp_mode = MTK_STP_MODE_FULL;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_set_full_unchecked(initial, 1),
			-EPROTONOSUPPORT);
	KUNIT_EXPECT_EQ(test, transport->set_count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_set_full_unchecked(initial, 1),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_mandatory_mode_required(struct kunit *test)
{
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;

	initial = mt6797_wmt_initial_test_create(test);
	transport = test->priv;
	transport->wmt.stp_mode = MTK_STP_MODE_FULL;
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPROTONOSUPPORT);
	KUNIT_EXPECT_EQ(test, transport->raw_count, 0U);
	KUNIT_EXPECT_EQ(test, transport->default_query_count, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
			-EPIPE);
	mt6797_consys_wmt_initial_destroy(initial);
}

static void mt6797_consys_wmt_initial_test_chip_full_errors_fail(struct kunit *test)
{
	static const struct {
		enum mt6797_consys_wmt_initial_test_reply reply;
		int expected;
	} failures[] = {
		{ MT6797_CONSYS_WMT_INITIAL_TEST_SET_MALFORMED, -EMSGSIZE },
		{ MT6797_CONSYS_WMT_INITIAL_TEST_SET_UNEXPECTED, -ETIMEDOUT },
		{ MT6797_CONSYS_WMT_INITIAL_TEST_SET_TIMEOUT, -ETIMEDOUT },
	};
	struct mt6797_consys_wmt_initial_test_transport *transport;
	struct mt6797_consys_wmt_initial *initial;
	struct mt6797_consys_wmt_initial_identity identity;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(failures); i++) {
		initial = mt6797_wmt_initial_test_create(test);
		transport = test->priv;
		KUNIT_ASSERT_EQ(test,
				mt6797_wmt_initial_run_unchecked(initial, 1, &identity),
				0);
		transport->reply = failures[i].reply;
		KUNIT_EXPECT_EQ(test,
				mt6797_wmt_initial_set_full_unchecked(initial, 1),
				failures[i].expected);
		KUNIT_EXPECT_EQ(test, transport->raw_count, 3U);
		KUNIT_EXPECT_EQ(test, transport->default_query_count, 1U);
		KUNIT_EXPECT_EQ(test, transport->set_count, 1U);
		KUNIT_EXPECT_EQ(test,
				mt6797_wmt_initial_set_full_unchecked(initial, 1),
				-EPIPE);
		mt6797_consys_wmt_initial_destroy(initial);
	}
}

static struct kunit_case mt6797_consys_wmt_initial_test_cases[] = {
	KUNIT_CASE(mt6797_consys_wmt_initial_test_sequence),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_raw_failure_fails),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_chip_rejection),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_hvr_rejection),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_query_errors_fail),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_chip_full_sequence),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_chip_full_requires_ready),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_mandatory_mode_required),
	KUNIT_CASE(mt6797_consys_wmt_initial_test_chip_full_errors_fail),
	{}
};

static struct kunit_suite mt6797_consys_wmt_initial_test_suite = {
	.name = "mt6797-consys-wmt-initial",
	.test_cases = mt6797_consys_wmt_initial_test_cases,
};

kunit_test_suite(mt6797_consys_wmt_initial_test_suite);

#endif /* CONFIG_MTK_MT6797_CONSYS_WMT_INITIAL_KUNIT_TEST */

MODULE_DESCRIPTION("MediaTek MT6797 identity and chip-side WMT STP controller");
MODULE_LICENSE("GPL");
