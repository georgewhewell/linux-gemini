// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>

#include <linux/errno.h>
#include <linux/soc/mediatek/mt6797-wifi-ahb-w3-session.h>

enum mt6797_wifi_w3_trace {
	W3_HIF_GET, W3_IRQ_REQUEST, W3_WHISR, W3_TX_RELEASED, W3_RX_LENGTHS,
	W3_RX0, W3_RX1, W3_CAPABILITY, W3_CONFIGURE, W3_WHCR_READ, W3_WHCR_WRITE,
	W3_INT_SET, W3_IRQ_ENABLE, W3_IRQ_DISABLE, W3_INT_CLR, W3_IRQ_FREE, W3_HIF_PUT,
};

struct mt6797_wifi_w3_fake {
	enum mt6797_wifi_w3_trace trace[64];
	unsigned int trace_count;
	u32 whcr;
	u32 whisr;
	u16 rx0;
	u16 rx1;
	int fail;
};

static int mt6797_wifi_w3_fake_trace(struct mt6797_wifi_w3_fake *fake,
			enum mt6797_wifi_w3_trace trace)
{
	fake->trace[fake->trace_count++] = trace;
	return fake->fail;
}

static int mt6797_wifi_w3_fake_hif_get(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_HIF_GET);
}

static void mt6797_wifi_w3_fake_hif_put(void *context)
{
	mt6797_wifi_w3_fake_trace(context, W3_HIF_PUT);
}

static int mt6797_wifi_w3_fake_irq_request(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_IRQ_REQUEST);
}

static void mt6797_wifi_w3_fake_irq_free(void *context)
{
	mt6797_wifi_w3_fake_trace(context, W3_IRQ_FREE);
}

static int mt6797_wifi_w3_fake_irq_enable(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_IRQ_ENABLE);
}

static int mt6797_wifi_w3_fake_irq_disable(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_IRQ_DISABLE);
}

static int mt6797_wifi_w3_fake_read(void *context, u32 offset, u32 *value)
{
	struct mt6797_wifi_w3_fake *fake = context;

	if (offset == MT6797_WIFI_AHB_W3_MCR_WHISR) {
		mt6797_wifi_w3_fake_trace(fake, W3_WHISR);
		*value = fake->whisr;
		return 0;
	}
	if (offset != MT6797_WIFI_AHB_W3_MCR_WHCR)
		return -EINVAL;
	mt6797_wifi_w3_fake_trace(fake, W3_WHCR_READ);
	*value = fake->whcr;
	return 0;
}

static int mt6797_wifi_w3_fake_write(void *context, u32 offset, u32 value)
{
	struct mt6797_wifi_w3_fake *fake = context;

	if (offset == MT6797_WIFI_AHB_W3_MCR_WHCR) {
		fake->whcr = value;
		return mt6797_wifi_w3_fake_trace(fake, W3_WHCR_WRITE);
	}
	if (offset != MT6797_WIFI_AHB_W3_MCR_WHLPCR)
		return -EINVAL;
	return mt6797_wifi_w3_fake_trace(fake,
		value == MT6797_WIFI_AHB_W3_WHLPCR_INT_SET ? W3_INT_SET : W3_INT_CLR);
}

static int mt6797_wifi_w3_fake_tx(void *context, u32 released[8])
{
	memset(released, 0, sizeof(u32) * 8);
	return mt6797_wifi_w3_fake_trace(context, W3_TX_RELEASED);
}

static int mt6797_wifi_w3_fake_lengths(void *context, u16 *rx0, u16 *rx1)
{
	struct mt6797_wifi_w3_fake *fake = context;

	mt6797_wifi_w3_fake_trace(fake, W3_RX_LENGTHS);
	*rx0 = fake->rx0;
	*rx1 = fake->rx1;
	return 0;
}

static int mt6797_wifi_w3_fake_drain(void *context, unsigned int port, u16 length)
{
	struct mt6797_wifi_w3_fake *fake = context;

	if (port == 0) {
		if (length != fake->rx0)
			return -EPROTO;
		fake->rx0 = 0;
		return mt6797_wifi_w3_fake_trace(fake, W3_RX0);
	}
	if (port != 1 || length != fake->rx1)
		return -EPROTO;
	fake->rx1 = 0;
	return mt6797_wifi_w3_fake_trace(fake, W3_RX1);
}

static int mt6797_wifi_w3_fake_capability(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_CAPABILITY);
}

static int mt6797_wifi_w3_fake_configure(void *context)
{
	return mt6797_wifi_w3_fake_trace(context, W3_CONFIGURE);
}

static const struct mt6797_wifi_ahb_w3_ops mt6797_wifi_w3_fake_ops = {
	.hif_get = mt6797_wifi_w3_fake_hif_get,
	.hif_put = mt6797_wifi_w3_fake_hif_put,
	.host_irq_request = mt6797_wifi_w3_fake_irq_request,
	.host_irq_free = mt6797_wifi_w3_fake_irq_free,
	.host_irq_enable = mt6797_wifi_w3_fake_irq_enable,
	.host_irq_disable = mt6797_wifi_w3_fake_irq_disable,
	.mcr_read = mt6797_wifi_w3_fake_read,
	.mcr_write = mt6797_wifi_w3_fake_write,
	.tx_released = mt6797_wifi_w3_fake_tx,
	.rx_lengths = mt6797_wifi_w3_fake_lengths,
	.rx_drain = mt6797_wifi_w3_fake_drain,
	.capability_query = mt6797_wifi_w3_fake_capability,
	.configure = mt6797_wifi_w3_fake_configure,
};

static void mt6797_wifi_w3_orders_enable_after_device_int(struct kunit *test)
{
	struct mt6797_wifi_w3_fake fake = {
		.whcr = MT6797_WIFI_AHB_W3_WHCR_INT_READ_CLEAR,
		.whisr = MT6797_WIFI_AHB_W3_WHISR_TX_DONE,
		.rx0 = 28, .rx1 = 124,
	};
	struct mt6797_wifi_ahb_w3_session session = { };

	KUNIT_ASSERT_EQ(test, mt6797_wifi_ahb_w3_session_start(&session,
		&mt6797_wifi_w3_fake_ops, &fake), 0);
	KUNIT_EXPECT_TRUE(test, session.active);
	KUNIT_EXPECT_EQ(test, session.tx_resource_resets, 1U);
	KUNIT_EXPECT_EQ(test, session.rx0_drained, 1U);
	KUNIT_EXPECT_EQ(test, session.rx1_drained, 1U);
	KUNIT_EXPECT_EQ(test, fake.trace[0], W3_HIF_GET);
	KUNIT_EXPECT_EQ(test, fake.trace[1], W3_IRQ_REQUEST);
	KUNIT_EXPECT_EQ(test, fake.trace[2], W3_WHISR);
	KUNIT_EXPECT_EQ(test, fake.trace[3], W3_TX_RELEASED);
	KUNIT_EXPECT_EQ(test, fake.trace[8], W3_CAPABILITY);
	KUNIT_EXPECT_EQ(test, fake.trace[9], W3_RX_LENGTHS);
	KUNIT_EXPECT_EQ(test, fake.trace[10], W3_CONFIGURE);
	KUNIT_EXPECT_EQ(test, fake.trace[13], W3_INT_SET);
	KUNIT_EXPECT_EQ(test, fake.trace[14], W3_IRQ_ENABLE);
	KUNIT_EXPECT_EQ(test, fake.whcr & MT6797_WIFI_AHB_W3_WHCR_INT_READ_CLEAR, 0U);
	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w3_session_stop(&session), 0);
	KUNIT_EXPECT_FALSE(test, session.active);
	KUNIT_EXPECT_EQ(test, fake.trace[15], W3_IRQ_DISABLE);
	KUNIT_EXPECT_EQ(test, fake.trace[16], W3_INT_CLR);
}

static void mt6797_wifi_w3_refuses_oversize_rx_before_irq(struct kunit *test)
{
	struct mt6797_wifi_w3_fake fake = {
		.rx0 = MT6797_WIFI_AHB_W3_RX_PACKET_MAX + 1,
	};
	struct mt6797_wifi_ahb_w3_session session = { };

	KUNIT_EXPECT_EQ(test, mt6797_wifi_ahb_w3_session_start(&session,
		&mt6797_wifi_w3_fake_ops, &fake), -EMSGSIZE);
	KUNIT_EXPECT_FALSE(test, session.active);
	KUNIT_EXPECT_EQ(test, fake.trace[2], W3_WHISR);
	KUNIT_EXPECT_EQ(test, fake.trace[3], W3_RX_LENGTHS);
	KUNIT_EXPECT_EQ(test, fake.trace[4], W3_WHISR);
	KUNIT_EXPECT_EQ(test, fake.trace[6], W3_IRQ_FREE);
	KUNIT_EXPECT_EQ(test, fake.trace[7], W3_HIF_PUT);
}

static struct kunit_case mt6797_wifi_w3_test_cases[] = {
	KUNIT_CASE(mt6797_wifi_w3_orders_enable_after_device_int),
	KUNIT_CASE(mt6797_wifi_w3_refuses_oversize_rx_before_irq),
	{}
};

static struct kunit_suite mt6797_wifi_w3_test_suite = {
	.name = "mt6797-wifi-ahb-w3-session",
	.test_cases = mt6797_wifi_w3_test_cases,
};

kunit_test_suite(mt6797_wifi_w3_test_suite);

MODULE_LICENSE("GPL");
