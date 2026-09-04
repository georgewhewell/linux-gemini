# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_MTK_MT6797_MODEM_PROTOCOL) += mt6797-md1-protocol.o
obj-$(CONFIG_MTK_MT6797_MD1) += mt6797-md1.o

mt6797-md1-protocol-y := protocol.o
mt6797-md1-protocol-$(CONFIG_MTK_MT6797_MODEM_PROTOCOL_KUNIT_TEST) += \
	protocol_test.o

mt6797-md1-y := cldma.o md1.o observer.o power.o
mt6797-md1-$(CONFIG_MTK_MT6797_MD1_KUNIT_TEST) += cldma_test.o
mt6797-md1-$(CONFIG_MTK_MT6797_MD1_KUNIT_TEST) += power_test.o
mt6797-md1-$(CONFIG_MTK_MT6797_MD1_KUNIT_TEST) += observer_test.o
