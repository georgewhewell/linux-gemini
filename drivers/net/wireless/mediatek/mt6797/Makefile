# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MT6797_WIFI_AHB_HIF) += mt6797-wifi-ahb-hif.o
mt6797-wifi-ahb-hif-y := hif.o
mt6797-wifi-ahb-hif-$(CONFIG_MT6797_WIFI_AHB_HIF_KUNIT_TEST) += hif_test.o

obj-$(CONFIG_MTK_MT6797_WIFI) += mt6797-wifi.o
mt6797-wifi-y := core.o firmware.o boot.o capability.o protocol.o \
	runtime_config.o hif_runtime.o hif_pump.o scan.o rx_metadata.o \
	scan_target.o ais_resources.o ais_protocol.o ais_runtime.o
mt6797-wifi-$(CONFIG_MTK_MT6797_WIFI_KUNIT_TEST) += firmware_test.o \
	boot_test.o capability_test.o protocol_test.o runtime_config_test.o \
	hif_runtime_test.o hif_pump_test.o scan_test.o rx_metadata_test.o \
	scan_target_test.o ais_resources_test.o ais_protocol_test.o \
	ais_runtime_test.o
