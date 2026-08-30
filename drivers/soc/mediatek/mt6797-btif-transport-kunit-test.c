// SPDX-License-Identifier: GPL-2.0-only
/* KUnit coverage for MT6797 BTIF/APDMA virtual-FIFO bookkeeping. */

#include <kunit/test.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/soc/mediatek/mt6797-btif.h>

static struct mt6797_btif_vfifo *mt6797_btif_test_fifo(struct kunit *test)
{
	struct mt6797_btif_vfifo *fifo;

	fifo = kunit_kzalloc(test, sizeof(*fifo), GFP_KERNEL);
	if (!fifo)
		return NULL;
	fifo->cpu_addr = kunit_kzalloc(test, MT6797_BTIF_VFIFO_SIZE, GFP_KERNEL);
	return fifo;
}

static void mt6797_btif_test_pointer_advance(struct kunit *test)
{
	u32 near_end = MT6797_BTIF_VFIFO_SIZE - 3;
	u32 next = 0;

	KUNIT_ASSERT_EQ(test, mt6797_btif_vfifo_advance(17, 31, &next), 0);
	KUNIT_EXPECT_EQ(test, next, 48U);
	KUNIT_ASSERT_EQ(test, mt6797_btif_vfifo_advance(near_end, 8, &next), 0);
	KUNIT_EXPECT_EQ(test, next, MT6797_BTIF_VFIFO_WRAP | 5U);
	KUNIT_EXPECT_EQ(test,
			mt6797_btif_vfifo_advance(MT6797_BTIF_VFIFO_SIZE, 1, &next),
			-EINVAL);
}

static void mt6797_btif_test_tx_short_space(struct kunit *test)
{
	static const u8 data[] = { 0x10, 0x20, 0x30, 0x40 };
	struct mt6797_btif_vfifo *fifo;
	u32 next = 0;

	fifo = mt6797_btif_test_fifo(test);
	KUNIT_ASSERT_NOT_NULL(test, fifo);
	KUNIT_ASSERT_NOT_NULL(test, fifo->cpu_addr);
	fifo->cpu_addr[7] = 0xa5;
	KUNIT_EXPECT_EQ(test,
			mt6797_btif_vfifo_tx_copy(fifo, 7, sizeof(data) - 1, data,
						  sizeof(data), &next), -EAGAIN);
	KUNIT_EXPECT_EQ(test, fifo->cpu_addr[7], 0xa5);
}

static void mt6797_btif_test_tx_wrap(struct kunit *test)
{
	static const u8 data[] = { 0x10, 0x20, 0x30, 0x40 };
	struct mt6797_btif_vfifo *fifo;
	u32 next = 0;

	fifo = mt6797_btif_test_fifo(test);
	KUNIT_ASSERT_NOT_NULL(test, fifo);
	KUNIT_ASSERT_NOT_NULL(test, fifo->cpu_addr);
	KUNIT_ASSERT_EQ(test,
			mt6797_btif_vfifo_tx_copy(fifo, MT6797_BTIF_VFIFO_SIZE - 2,
						  sizeof(data), data, sizeof(data),
						  &next), 0);
	KUNIT_EXPECT_EQ(test, next, MT6797_BTIF_VFIFO_WRAP | 2U);
	KUNIT_EXPECT_EQ(test, fifo->wpt, next);
	KUNIT_EXPECT_EQ(test, fifo->cpu_addr[MT6797_BTIF_VFIFO_SIZE - 2], data[0]);
	KUNIT_EXPECT_EQ(test, fifo->cpu_addr[MT6797_BTIF_VFIFO_SIZE - 1], data[1]);
	KUNIT_EXPECT_EQ(test, fifo->cpu_addr[0], data[2]);
	KUNIT_EXPECT_EQ(test, fifo->cpu_addr[1], data[3]);
}

static void mt6797_btif_test_rx_wrap_segments(struct kunit *test)
{
	struct mt6797_btif_vfifo *fifo;
	struct mt6797_btif_rx_segments segments;

	fifo = mt6797_btif_test_fifo(test);
	KUNIT_ASSERT_NOT_NULL(test, fifo);
	KUNIT_ASSERT_NOT_NULL(test, fifo->cpu_addr);
	fifo->rpt = MT6797_BTIF_VFIFO_SIZE - 3;
	KUNIT_ASSERT_EQ(test,
			mt6797_btif_vfifo_rx_segments(fifo, MT6797_BTIF_VFIFO_WRAP | 4,
						      &segments), 0);
	KUNIT_EXPECT_PTR_EQ(test, segments.first,
			    fifo->cpu_addr + MT6797_BTIF_VFIFO_SIZE - 3);
	KUNIT_EXPECT_EQ(test, segments.first_len, 3UL);
	KUNIT_EXPECT_PTR_EQ(test, segments.second, fifo->cpu_addr);
	KUNIT_EXPECT_EQ(test, segments.second_len, 4UL);
	KUNIT_EXPECT_EQ(test, segments.next_rpt,
			MT6797_BTIF_VFIFO_WRAP | 4U);
}

static void mt6797_btif_test_rx_invalid_progress(struct kunit *test)
{
	struct mt6797_btif_vfifo *fifo;
	struct mt6797_btif_rx_segments segments;

	fifo = mt6797_btif_test_fifo(test);
	KUNIT_ASSERT_NOT_NULL(test, fifo);
	KUNIT_ASSERT_NOT_NULL(test, fifo->cpu_addr);
	fifo->rpt = 32;
	KUNIT_EXPECT_EQ(test,
			mt6797_btif_vfifo_rx_segments(fifo, 31, &segments),
			-EOVERFLOW);
}

static struct kunit_case mt6797_btif_transport_test_cases[] = {
	KUNIT_CASE(mt6797_btif_test_pointer_advance),
	KUNIT_CASE(mt6797_btif_test_tx_short_space),
	KUNIT_CASE(mt6797_btif_test_tx_wrap),
	KUNIT_CASE(mt6797_btif_test_rx_wrap_segments),
	KUNIT_CASE(mt6797_btif_test_rx_invalid_progress),
	{}
};

static struct kunit_suite mt6797_btif_transport_test_suite = {
	.name = "mt6797-btif-transport",
	.test_cases = mt6797_btif_transport_test_cases,
};

kunit_test_suite(mt6797_btif_transport_test_suite);

MODULE_LICENSE("GPL");
