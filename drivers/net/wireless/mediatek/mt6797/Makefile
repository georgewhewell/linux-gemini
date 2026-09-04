# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MT6797_WIFI_AHB_HIF) += mt6797-wifi-ahb-hif.o
mt6797-wifi-ahb-hif-y := hif.o
mt6797-wifi-ahb-hif-$(CONFIG_MT6797_WIFI_AHB_HIF_KUNIT_TEST) += hif_test.o

obj-$(CONFIG_MTK_MT6797_WIFI) += mt6797-wifi.o
mt6797-wifi-y := core.o firmware.o boot.o capability.o
mt6797-wifi-$(CONFIG_MTK_MT6797_WIFI_KUNIT_TEST) += firmware_test.o boot_test.o capability_test.o
