// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 BTIF mandatory-to-full STP bootstrap controller.
 *
 * The retained Gemini vendor sequence is deliberately the whole contract:
 *
 * - wmt_core.c:747-809 selects BTIF mandatory mode before this sequence;
 * - wmt_ic_soc.c:122-124 defines the query command and both exact replies;
 * - wmt_ic_soc.c:190-191 defines the exact set command/reply;
 * - wmt_ic_soc.c:1008-1044 executes query-default, set, host full-mode,
 *   waits 10 ms (with >=2 ms documented), then query-full;
 * - wmt_core.c:634-673 sends every init-script command then compares its
 *   complete reply, and wmt_ctrl.c:248-286 uses WMT_LIB_RX_TIMEOUT;
 * - include/wmt_lib.h:70 defines that reply timeout as 2000 ms;
 * - stp_core.c:577-604 and 632-754 establish the full STP 0/7 initial window
 *   and 180-ms retained-frame resync/retry behavior.
 *
 * This is neither a CONSYS power sequencer nor a radio-function controller.
 * It performs no discovery and has no DT match.  The only transmission path
 * is default-false parameter gated, and only the three vendor STP-switch WMT
 * exchanges below can be emitted.  In particular, it neither downloads a
 * patch nor enables Wi-Fi, Bluetooth, GNSS, or FM.
 */

#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/kthread.h>
#endif

#include <linux/soc/mediatek/mt6797-btif-pio.h>
#include <linux/soc/mediatek/mt6797-btif-stp-bootstrap.h>
#include <linux/soc/mediatek/mt6797-consys-stp-session.h>
#include <linux/soc/mediatek/mtk-stp.h>
#include <linux/soc/mediatek/mtk-wmt.h>

#define MT6797_BTIF_STP_WMT_OPCODE		0x04
#define MT6797_BTIF_STP_WMT_TIMEOUT_MS		2000
#define MT6797_BTIF_STP_HOST_SWITCH_DELAY_MS	10

static bool enable_experimental_stp_bootstrap;
module_param_named(enable_experimental_stp_bootstrap,
			   enable_experimental_stp_bootstrap, bool, 0400);
MODULE_PARM_DESC(enable_experimental_stp_bootstrap,
		 "allow the dormant MT6797 BTIF STP bootstrap experiment (default: false)");

enum mt6797_btif_stp_bootstrap_state {
	MT6797_BTIF_STP_BOOTSTRAP_NEW,
	MT6797_BTIF_STP_BOOTSTRAP_MANDATORY,
	MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY,
	MT6797_BTIF_STP_BOOTSTRAP_ACTIVE,
	MT6797_BTIF_STP_BOOTSTRAP_FENCED,
};

struct mt6797_btif_stp_bootstrap {
	struct mutex lifecycle_lock;
	/* destroy() waits for the sole runner before it frees this object. */
	struct completion run_complete;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mtk_stp mandatory_stp;
	struct mtk_wmt mandatory_wmt;
	struct mtk_wmt full_wmt;
	struct mt6797_consys_stp_session *session;
	const struct mt6797_consys_stp_session_bootstrap_lease *full_bootstrap;
	const struct mt6797_consys_stp_session_lease *full_session;
	struct delayed_work full_retry_work;
	bool full_retry_armed;
	bool run_in_progress;
	bool destroy_requested;
	enum mt6797_btif_stp_bootstrap_state state;
};

/* Whole vendor WMT packets, split into command SDU and expected event SDU. */
static const u8 mt6797_btif_stp_query_data[] = { 0x04 };
static const u8 mt6797_btif_stp_query_default_data[] = {
	0x00, 0x04, 0x11, 0x00, 0x00, 0x00,
};
static const u8 mt6797_btif_stp_set_data[] = {
	0x03, 0xdf, 0x0e, 0x68, 0x01,
};
static const u8 mt6797_btif_stp_set_reply_data[] = { 0x00, 0x03 };
static const u8 mt6797_btif_stp_query_full_data[] = {
	0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01,
};

static void mt6797_btif_stp_bootstrap_fence(
		struct mt6797_btif_stp_bootstrap *bootstrap, bool from_retry_work)
{
	bool started;

	mutex_lock(&bootstrap->lifecycle_lock);
	if (bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FENCED) {
		mutex_unlock(&bootstrap->lifecycle_lock);
		/* A retry worker may have set FENCED before finishing close(). */
		if (!from_retry_work)
			cancel_delayed_work_sync(&bootstrap->full_retry_work);
		return;
	}
	started = bootstrap->state != MT6797_BTIF_STP_BOOTSTRAP_NEW;
	bootstrap->full_retry_armed = false;
	bootstrap->state = MT6797_BTIF_STP_BOOTSTRAP_FENCED;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (!from_retry_work)
		cancel_delayed_work_sync(&bootstrap->full_retry_work);
	mt6797_consys_stp_session_quiesce(bootstrap->session);
	/* close() is the only supplied path that synchronizes the Linux IRQ. */
	if (started)
		bootstrap->ops.close(bootstrap->ops.context);
}

static int mt6797_btif_stp_bootstrap_receive(void *context, const u8 *data,
					      size_t length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;

	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_MANDATORY)
		return mtk_stp_rx(&bootstrap->mandatory_stp, data, length);
	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return mt6797_consys_stp_session_bootstrap_receive(
				bootstrap->full_bootstrap, data, length);
	if (READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_ACTIVE)
		return mt6797_consys_stp_session_receive_lease(
				bootstrap->full_session, data, length);
	return -ESHUTDOWN;
}

static int mt6797_btif_stp_bootstrap_full_wmt_receive(void *context,
							      enum mtk_stp_channel channel,
							      const u8 *payload, size_t payload_length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;

	if (channel != MTK_STP_WMT)
		return -EOPNOTSUPP;
	return mtk_wmt_receive(&bootstrap->full_wmt, payload, payload_length);
}

static int mt6797_btif_stp_bootstrap_full_wmt_transmit(void *context,
							       const u8 *packet, size_t packet_length)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = context;
	int ret;

	if (READ_ONCE(bootstrap->state) != MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return -ESHUTDOWN;
	ret = mt6797_consys_stp_session_bootstrap_transmit(
		bootstrap->full_bootstrap, MTK_STP_WMT, packet, packet_length);
	if (!ret && READ_ONCE(bootstrap->full_retry_armed))
		schedule_delayed_work(&bootstrap->full_retry_work,
			msecs_to_jiffies(MT6797_CONSYS_STP_TX_TIMEOUT_MS));
	return ret;
}

static void mt6797_btif_stp_bootstrap_full_retry(struct work_struct *work)
{
	struct mt6797_btif_stp_bootstrap *bootstrap = container_of(
		to_delayed_work(work), struct mt6797_btif_stp_bootstrap,
		full_retry_work);
	int ret;

	if (!READ_ONCE(bootstrap->full_retry_armed) ||
	    READ_ONCE(bootstrap->state) != MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		return;

	ret = mt6797_consys_stp_session_bootstrap_timeout(bootstrap->full_bootstrap);
	if (ret) {
		mt6797_btif_stp_bootstrap_fence(bootstrap, true);
		return;
	}
	if (READ_ONCE(bootstrap->full_retry_armed) &&
	    READ_ONCE(bootstrap->state) == MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY)
		schedule_delayed_work(&bootstrap->full_retry_work,
			msecs_to_jiffies(MT6797_CONSYS_STP_TX_TIMEOUT_MS));
}

struct mt6797_btif_stp_bootstrap *
mt6797_btif_stp_bootstrap_create(
		const struct mt6797_btif_pio_lease *pio_lease,
		const struct mt6797_btif_stp_bootstrap_ops *ops)
{
	struct mt6797_btif_stp_bootstrap *bootstrap;

	if (!pio_lease || !ops || !ops->enable_irq || !ops->close)
		return ERR_PTR(-EINVAL);

	bootstrap = kzalloc_obj(*bootstrap, GFP_KERNEL);
	if (!bootstrap)
		return ERR_PTR(-ENOMEM);
	mutex_init(&bootstrap->lifecycle_lock);
	init_completion(&bootstrap->run_complete);
	INIT_DELAYED_WORK(&bootstrap->full_retry_work,
			  mt6797_btif_stp_bootstrap_full_retry);
	bootstrap->pio_lease = pio_lease;
	bootstrap->ops = *ops;
	return bootstrap;
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_create);

static const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run_unchecked(
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	const struct mt6797_consys_stp_session_lease *session_lease;
	struct mt6797_consys_stp_session_ops session_ops;
	struct mtk_wmt_match expected;
	int ret;

	mutex_lock(&bootstrap->lifecycle_lock);
	if (bootstrap->destroy_requested) {
		session_lease = ERR_PTR(-ESHUTDOWN);
		goto out_unlock;
	}
	if (bootstrap->state == MT6797_BTIF_STP_BOOTSTRAP_FENCED) {
		session_lease = ERR_PTR(-EPIPE);
		goto out_unlock;
	}
	if (bootstrap->state != MT6797_BTIF_STP_BOOTSTRAP_NEW) {
		session_lease = ERR_PTR(-EBUSY);
		goto out_unlock;
	}
	bootstrap->run_in_progress = true;
	reinit_completion(&bootstrap->run_complete);

	mtk_stp_init(&bootstrap->mandatory_stp, MTK_STP_MODE_MANDATORY);
	mtk_wmt_init(&bootstrap->mandatory_wmt, MTK_STP_MODE_MANDATORY,
		     mt6797_btif_pio_transmit, (void *)bootstrap->pio_lease);
	ret = mtk_stp_register_client(&bootstrap->mandatory_stp, MTK_STP_WMT,
				     mtk_wmt_receive, &bootstrap->mandatory_wmt);
	if (ret)
		goto fail_locked;
	ret = mt6797_btif_pio_begin_mandatory(bootstrap->pio_lease,
					      mt6797_btif_stp_bootstrap_receive,
					      bootstrap);
	if (ret)
		goto fail_locked;
	/* The RX consumer is valid before an installed IRQ becomes deliverable. */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_MANDATORY);
	ret = bootstrap->ops.enable_irq(bootstrap->ops.context);
	if (ret)
		goto fail_locked;
	mutex_unlock(&bootstrap->lifecycle_lock);

	expected = (struct mtk_wmt_match) {
		.opcode = MT6797_BTIF_STP_WMT_OPCODE,
		.data = mt6797_btif_stp_query_default_data,
		.data_len = sizeof(mt6797_btif_stp_query_default_data),
	};
	ret = mtk_wmt_transaction(&bootstrap->mandatory_wmt,
			  MT6797_BTIF_STP_WMT_OPCODE, mt6797_btif_stp_query_data,
			  sizeof(mt6797_btif_stp_query_data), &expected,
			  msecs_to_jiffies(MT6797_BTIF_STP_WMT_TIMEOUT_MS));
	if (ret)
		goto fail;
	expected = (struct mtk_wmt_match) {
		.opcode = MT6797_BTIF_STP_WMT_OPCODE,
		.data = mt6797_btif_stp_set_reply_data,
		.data_len = sizeof(mt6797_btif_stp_set_reply_data),
	};
	ret = mtk_wmt_transaction(&bootstrap->mandatory_wmt,
			  MT6797_BTIF_STP_WMT_OPCODE, mt6797_btif_stp_set_data,
			  sizeof(mt6797_btif_stp_set_data), &expected,
			  msecs_to_jiffies(MT6797_BTIF_STP_WMT_TIMEOUT_MS));
	if (ret)
		goto fail;

	session_ops = (struct mt6797_consys_stp_session_ops) {
		.transmit = mt6797_btif_pio_transmit,
		.receive = mt6797_btif_stp_bootstrap_full_wmt_receive,
		.transmit_context = (void *)bootstrap->pio_lease,
		.receive_context = bootstrap,
	};
	bootstrap->session = mt6797_consys_stp_session_create(&session_ops);
	if (IS_ERR(bootstrap->session)) {
		ret = PTR_ERR(bootstrap->session);
		bootstrap->session = NULL;
		goto fail;
	}
	bootstrap->full_bootstrap =
		mt6797_consys_stp_session_begin_full_bootstrap(bootstrap->session);
	if (IS_ERR(bootstrap->full_bootstrap)) {
		ret = PTR_ERR(bootstrap->full_bootstrap);
		bootstrap->full_bootstrap = NULL;
		goto fail;
	}
	mtk_wmt_init_payload_transport(&bootstrap->full_wmt, MTK_STP_MODE_FULL,
				       mt6797_btif_stp_bootstrap_full_wmt_transmit,
				       bootstrap);
	/*
	 * PIO keeps this router from the mandatory phase onwards.  Publishing the
	 * full parser first gives every RX byte one unambiguous owner while PIO
	 * serializes its own state transition; there is no callback/state gap.
	 */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_FULL_VERIFY);
	ret = mt6797_btif_pio_handoff_full_bootstrap(bootstrap->pio_lease,
					     mt6797_btif_stp_bootstrap_receive,
					     bootstrap);
	if (ret)
		goto fail;
	/* Vendor wmt_ic_soc.c says 10 ms, although its stated minimum is 2 ms. */
	msleep(MT6797_BTIF_STP_HOST_SWITCH_DELAY_MS);

	expected = (struct mtk_wmt_match) {
		.opcode = MT6797_BTIF_STP_WMT_OPCODE,
		.data = mt6797_btif_stp_query_full_data,
		.data_len = sizeof(mt6797_btif_stp_query_full_data),
	};
	WRITE_ONCE(bootstrap->full_retry_armed, true);
	ret = mtk_wmt_transaction(&bootstrap->full_wmt,
			  MT6797_BTIF_STP_WMT_OPCODE, mt6797_btif_stp_query_data,
			  sizeof(mt6797_btif_stp_query_data), &expected,
			  msecs_to_jiffies(MT6797_BTIF_STP_WMT_TIMEOUT_MS));
	WRITE_ONCE(bootstrap->full_retry_armed, false);
	cancel_delayed_work_sync(&bootstrap->full_retry_work);
	if (ret)
		goto fail;

	/* mtk_wmt_transaction() returned zero only after its exact reply matcher. */
	ret = mt6797_consys_stp_session_bootstrap_confirm(
		bootstrap->full_bootstrap);
	if (ret)
		goto fail;
	bootstrap->full_session = mt6797_consys_stp_session_bootstrap_promote(
		bootstrap->full_bootstrap);
	if (IS_ERR(bootstrap->full_session)) {
		ret = PTR_ERR(bootstrap->full_session);
		bootstrap->full_session = NULL;
		goto fail;
	}
	/* The stable router changes state before PIO's final commit. */
	WRITE_ONCE(bootstrap->state, MT6797_BTIF_STP_BOOTSTRAP_ACTIVE);
	ret = mt6797_btif_pio_promote_full(bootstrap->pio_lease,
					      mt6797_btif_stp_bootstrap_receive,
					      bootstrap);
	if (ret)
		goto fail;
	session_lease = bootstrap->full_session;
	goto out_complete;

fail:
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	session_lease = ERR_PTR(ret);
	goto out_complete;
fail_locked:
	mutex_unlock(&bootstrap->lifecycle_lock);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	session_lease = ERR_PTR(ret);
	goto out_complete;
out_complete:
	mutex_lock(&bootstrap->lifecycle_lock);
	bootstrap->run_in_progress = false;
	complete_all(&bootstrap->run_complete);
	mutex_unlock(&bootstrap->lifecycle_lock);
	return session_lease;
out_unlock:
	mutex_unlock(&bootstrap->lifecycle_lock);
	return session_lease;
}

const struct mt6797_consys_stp_session_lease *
mt6797_btif_stp_bootstrap_run(struct mt6797_btif_stp_bootstrap *bootstrap)
{
	if (!bootstrap)
		return ERR_PTR(-EINVAL);
	if (!READ_ONCE(enable_experimental_stp_bootstrap))
		return ERR_PTR(-EPERM);
	return mt6797_btif_stp_bootstrap_run_unchecked(bootstrap);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_run);

void mt6797_btif_stp_bootstrap_destroy(
		struct mt6797_btif_stp_bootstrap *bootstrap)
{
	bool run_in_progress;

	if (!bootstrap || IS_ERR(bootstrap))
		return;

	/*
	 * run() drops lifecycle_lock around bounded WMT transactions.  Publish the
	 * terminal intent under that lock and wait for that runner before fencing
	 * or freeing the embedded WMT/session objects it still uses.  The supplied
	 * close() callback must not re-enter destroy().
	 */
	mutex_lock(&bootstrap->lifecycle_lock);
	bootstrap->destroy_requested = true;
	run_in_progress = bootstrap->run_in_progress;
	mutex_unlock(&bootstrap->lifecycle_lock);
	if (run_in_progress)
		wait_for_completion(&bootstrap->run_complete);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	mt6797_consys_stp_session_destroy(bootstrap->session);
	kfree(bootstrap);
}
EXPORT_SYMBOL_GPL(mt6797_btif_stp_bootstrap_destroy);

#if IS_ENABLED(CONFIG_MTK_MT6797_BTIF_STP_BOOTSTRAP_KUNIT_TEST)

#define MT6797_BTIF_STP_TEST_IER		0x04
#define MT6797_BTIF_STP_TEST_IIR		0x08
#define MT6797_BTIF_STP_TEST_LSR		0x14
#define MT6797_BTIF_STP_TEST_IIR_NINT		BIT(0)
#define MT6797_BTIF_STP_TEST_IIR_TX_EMPTY	BIT(1)
#define MT6797_BTIF_STP_TEST_IIR_RX		BIT(2)
#define MT6797_BTIF_STP_TEST_LSR_TEMT		BIT(6)
#define MT6797_BTIF_STP_TEST_REGS		(0x70 / sizeof(u32))

struct mt6797_btif_stp_test_bus {
	u32 regs[MT6797_BTIF_STP_TEST_REGS];
	u32 iir;
	u8 rx[64];
	size_t rx_length;
	size_t rx_position;
	u8 tx[128];
	size_t tx_length;
};

struct mt6797_btif_stp_test_peer {
	struct mt6797_btif_stp_test_bus *bus;
	const struct mt6797_btif_pio_lease *pio_lease;
	struct mt6797_btif_pio *pio;
	struct task_struct *task;
	struct completion done;
	size_t tx_position;
	unsigned int stage;
	unsigned int close_count;
	int error;
};

struct mt6797_btif_stp_test_destroy_race {
	struct mt6797_btif_stp_bootstrap *bootstrap;
	struct mt6797_btif_pio *pio;
	struct completion enable_irq_entered;
	struct completion allow_enable_irq;
	struct completion run_done;
	struct completion destroy_done;
	unsigned int close_count;
	int run_result;
};

static u32 mt6797_btif_stp_test_read32(void *context, u32 offset)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset == MT6797_BTIF_STP_TEST_IIR) {
		if (bus->rx_position < bus->rx_length)
			return MT6797_BTIF_STP_TEST_IIR_RX;
		return bus->iir;
	}
	return bus->regs[offset / sizeof(u32)];
}

static void mt6797_btif_stp_test_write32(void *context, u32 offset, u32 value)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	bus->regs[offset / sizeof(u32)] = value;
}

static u8 mt6797_btif_stp_test_read8(void *context, u32 offset)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset != 0 || bus->rx_position == bus->rx_length)
		return 0;
	return bus->rx[bus->rx_position++];
}

static void mt6797_btif_stp_test_write8(void *context, u32 offset, u8 value)
{
	struct mt6797_btif_stp_test_bus *bus = context;

	if (offset == 0 && bus->tx_length < sizeof(bus->tx))
		bus->tx[bus->tx_length++] = value;
}

static int mt6797_btif_stp_test_frame(unsigned int stage, bool reply,
				      u8 *frame, size_t frame_size, size_t *frame_length)
{
	const u8 *packet;
	size_t packet_length;
	enum mtk_stp_mode mode = MTK_STP_MODE_MANDATORY;
	u8 sequence = 0;
	u8 ack = 0;
	static const u8 commands[][9] = {
		{ 0x01, 0x04, 0x01, 0x00, 0x04 },
		{ 0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01 },
		{ 0x01, 0x04, 0x01, 0x00, 0x04 },
	};
	static const u8 command_lengths[] = { 5, 9, 5 };
	static const u8 replies[][10] = {
		{ 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00 },
		{ 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 },
		{ 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01 },
	};
	static const u8 reply_lengths[] = { 10, 6, 10 };

	if (stage >= ARRAY_SIZE(commands))
		return -EINVAL;
	if (stage == 2) {
		mode = MTK_STP_MODE_FULL;
		if (reply)
			ack = 0;
		else
			ack = 7;
	}
	if (reply) {
		packet = replies[stage];
		packet_length = reply_lengths[stage];
	} else {
		packet = commands[stage];
		packet_length = command_lengths[stage];
	}
	return mtk_stp_encode(mode, MTK_STP_WMT, sequence, ack, packet,
			      packet_length, frame, frame_size, frame_length);
}

static int mt6797_btif_stp_test_peer_thread(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;
	u8 expected[32];
	u8 reply[32];
	size_t expected_length;
	size_t reply_length;
	int ret;

	while (!kthread_should_stop() && peer->stage < 3) {
		peer->bus->iir = MT6797_BTIF_STP_TEST_IIR_TX_EMPTY;
		peer->bus->regs[MT6797_BTIF_STP_TEST_LSR / sizeof(u32)] =
			MT6797_BTIF_STP_TEST_LSR_TEMT;
		ret = mt6797_btif_pio_irq(peer->pio_lease);
		if (ret && ret != -EAGAIN) {
			peer->error = ret;
			break;
		}
		ret = mt6797_btif_stp_test_frame(peer->stage, false, expected,
						 sizeof(expected), &expected_length);
		if (ret || peer->bus->tx_length - peer->tx_position < expected_length) {
			if (ret)
				peer->error = ret;
			usleep_range(1000, 2000);
			continue;
		}
		if (memcmp(peer->bus->tx + peer->tx_position, expected,
			   expected_length)) {
			peer->error = -EPROTO;
			break;
		}
		peer->tx_position += expected_length;
		ret = mt6797_btif_stp_test_frame(peer->stage, true, reply,
						 sizeof(reply), &reply_length);
		if (ret || reply_length > sizeof(peer->bus->rx)) {
			peer->error = ret ?: -EMSGSIZE;
			break;
		}
		memcpy(peer->bus->rx, reply, reply_length);
		peer->bus->rx_length = reply_length;
		peer->bus->rx_position = 0;
		peer->bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
		ret = mt6797_btif_pio_irq(peer->pio_lease);
		if (ret) {
			peer->error = ret;
			break;
		}
		peer->stage++;
	}
	complete(&peer->done);
	return 0;
}

static int mt6797_btif_stp_test_enable_irq(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;

	peer->task = kthread_run(mt6797_btif_stp_test_peer_thread, peer,
				 "mt6797-stp-peer");
	return IS_ERR(peer->task) ? PTR_ERR(peer->task) : 0;
}

static int mt6797_btif_stp_test_block_enable_irq(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	complete(&race->enable_irq_entered);
	wait_for_completion(&race->allow_enable_irq);
	return -EIO;
}

static void mt6797_btif_stp_test_destroy_race_close(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	race->close_count++;
	mt6797_btif_pio_close(race->pio);
}

static int mt6797_btif_stp_test_run_thread(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;
	const struct mt6797_consys_stp_session_lease *session_lease;

	session_lease = mt6797_btif_stp_bootstrap_run_unchecked(race->bootstrap);
	race->run_result = IS_ERR(session_lease) ? PTR_ERR(session_lease) : 0;
	complete(&race->run_done);
	return 0;
}

static int mt6797_btif_stp_test_destroy_thread(void *context)
{
	struct mt6797_btif_stp_test_destroy_race *race = context;

	mt6797_btif_stp_bootstrap_destroy(race->bootstrap);
	complete(&race->destroy_done);
	return 0;
}

static void mt6797_btif_stp_test_close(void *context)
{
	struct mt6797_btif_stp_test_peer *peer = context;

	peer->close_count++;
	if (peer->task && !IS_ERR(peer->task))
		kthread_stop(peer->task);
	mt6797_btif_pio_close(peer->pio);
}

static void mt6797_btif_stp_test_exact_handoff(struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;
	const struct mt6797_consys_stp_session_lease *session_lease;
	unsigned long waited;

	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
	init_completion(&peer->done);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_enable_irq,
		.close = mt6797_btif_stp_test_close,
		.context = peer,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	session_lease = mt6797_btif_stp_bootstrap_run_unchecked(bootstrap);
	KUNIT_ASSERT_FALSE(test, IS_ERR(session_lease));
	waited = wait_for_completion_timeout(&peer->done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, peer->error, 0);
	KUNIT_EXPECT_EQ(test, peer->stage, 3U);
	KUNIT_EXPECT_TRUE(test,
		mt6797_consys_stp_session_lease_active(session_lease));
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	mt6797_btif_pio_destroy(peer->pio);
}

static void mt6797_btif_stp_test_fence_is_one_shot(struct kunit *test)
{
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct mt6797_btif_stp_bootstrap *bootstrap;

	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_enable_irq,
		.close = mt6797_btif_stp_test_close,
		.context = peer,
	};
	bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(bootstrap));
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	KUNIT_EXPECT_EQ(test, peer->close_count, 0U);
	bootstrap->state = MT6797_BTIF_STP_BOOTSTRAP_MANDATORY;
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	mt6797_btif_stp_bootstrap_fence(bootstrap, false);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	mt6797_btif_stp_bootstrap_destroy(bootstrap);
	KUNIT_EXPECT_EQ(test, peer->close_count, 1U);
	mt6797_btif_pio_destroy(peer->pio);
}

static void mt6797_btif_stp_test_destroy_waits_for_runner(struct kunit *test)
{
	struct mt6797_btif_stp_test_destroy_race *race;
	struct mt6797_btif_stp_test_peer *peer;
	struct mt6797_btif_stp_test_bus *bus;
	struct mt6797_btif_pio_io_ops io_ops;
	struct mt6797_btif_stp_bootstrap_ops ops;
	struct task_struct *destroy_task;
	struct task_struct *run_task;
	unsigned long waited;

	race = kunit_kzalloc(test, sizeof(*race), GFP_KERNEL);
	peer = kunit_kzalloc(test, sizeof(*peer), GFP_KERNEL);
	bus = kunit_kzalloc(test, sizeof(*bus), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, race);
	KUNIT_ASSERT_NOT_NULL(test, peer);
	KUNIT_ASSERT_NOT_NULL(test, bus);
	peer->bus = bus;
	bus->iir = MT6797_BTIF_STP_TEST_IIR_NINT;
	init_completion(&race->enable_irq_entered);
	init_completion(&race->allow_enable_irq);
	init_completion(&race->run_done);
	init_completion(&race->destroy_done);
	io_ops = (struct mt6797_btif_pio_io_ops) {
		.read32 = mt6797_btif_stp_test_read32,
		.write32 = mt6797_btif_stp_test_write32,
		.read8 = mt6797_btif_stp_test_read8,
		.write8 = mt6797_btif_stp_test_write8,
		.context = bus,
	};
	peer->pio = mt6797_btif_pio_create(&io_ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio));
	race->pio = peer->pio;
	peer->pio_lease = mt6797_btif_pio_acquire(peer->pio);
	KUNIT_ASSERT_FALSE(test, IS_ERR(peer->pio_lease));
	ops = (struct mt6797_btif_stp_bootstrap_ops) {
		.enable_irq = mt6797_btif_stp_test_block_enable_irq,
		.close = mt6797_btif_stp_test_destroy_race_close,
		.context = race,
	};
	race->bootstrap = mt6797_btif_stp_bootstrap_create(peer->pio_lease, &ops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(race->bootstrap));

	run_task = kthread_run(mt6797_btif_stp_test_run_thread, race,
				       "mt6797-stp-run");
	KUNIT_ASSERT_FALSE(test, IS_ERR(run_task));
	waited = wait_for_completion_timeout(&race->enable_irq_entered,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	destroy_task = kthread_run(mt6797_btif_stp_test_destroy_thread, race,
					   "mt6797-stp-destroy");
	KUNIT_ASSERT_FALSE(test, IS_ERR(destroy_task));
	/* destroy() must remain blocked until the in-flight runner returns. */
	waited = wait_for_completion_timeout(&race->destroy_done,
					     msecs_to_jiffies(20));
	KUNIT_EXPECT_EQ(test, waited, 0UL);
	complete(&race->allow_enable_irq);
	waited = wait_for_completion_timeout(&race->run_done, msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	waited = wait_for_completion_timeout(&race->destroy_done,
					     msecs_to_jiffies(1000));
	KUNIT_ASSERT_NE(test, waited, 0UL);
	KUNIT_EXPECT_EQ(test, race->run_result, -EIO);
	KUNIT_EXPECT_EQ(test, race->close_count, 1U);
	mt6797_btif_pio_destroy(peer->pio);
}

static struct kunit_case mt6797_btif_stp_bootstrap_test_cases[] = {
	KUNIT_CASE(mt6797_btif_stp_test_exact_handoff),
	KUNIT_CASE(mt6797_btif_stp_test_fence_is_one_shot),
	KUNIT_CASE(mt6797_btif_stp_test_destroy_waits_for_runner),
	{ }
};

static struct kunit_suite mt6797_btif_stp_bootstrap_test_suite = {
	.name = "mt6797-btif-stp-bootstrap",
	.test_cases = mt6797_btif_stp_bootstrap_test_cases,
};

kunit_test_suite(mt6797_btif_stp_bootstrap_test_suite);

#endif

MODULE_DESCRIPTION("Dormant MT6797 BTIF mandatory-to-full STP handover");
MODULE_LICENSE("GPL");
