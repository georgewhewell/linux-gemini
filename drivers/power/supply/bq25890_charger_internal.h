/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _BQ25890_CHARGER_INTERNAL_H
#define _BQ25890_CHARGER_INTERNAL_H

#include <linux/types.h>

enum bq25890_control_mode {
	BQ25890_CONTROL_NORMAL,
	BQ25890_CONTROL_READ_ONLY,
	BQ25890_CONTROL_ADOPT_FIRMWARE,
};

int bq25890_control_mode_from_flags(bool read_only, bool adopt_firmware,
				    enum bq25890_control_mode *mode);
bool bq25890_control_mode_is_locked(enum bq25890_control_mode mode);
bool bq25890_control_mode_allows_write(enum bq25890_control_mode mode,
				       bool watchdog_reset);
unsigned int bq25890_adopt_watchdog_delay_ms(u8 setting);

#endif /* _BQ25890_CHARGER_INTERNAL_H */
