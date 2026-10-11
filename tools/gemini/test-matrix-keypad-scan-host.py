#!/usr/bin/env python3
"""Run the production matrix scan against a GPIO matrix with injected errors."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "drivers/input/keyboard/matrix_keypad.c").read_text()
production = source[source.index("struct matrix_keypad {"):source.index("static irqreturn_t matrix_keypad_interrupt")]

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef int spinlock_t;
#define MATRIX_MAX_ROWS 32
#define MATRIX_MAX_COLS 32
#define BIT(n) (1UL << (n))
#define DECLARE_BITMAP(n, bits) unsigned long n[((bits) + 63) / 64]
#define MATRIX_SCAN_CODE(row, col, shift) (((row) << (shift)) + (col))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define unlikely(x) (x)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define scoped_guard(kind, lock) for (int once = 1; once; once = 0)
#define guard(kind) fake_guard
#define EV_MSC 4
#define MSC_SCAN 4
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct input_dev { const unsigned short *keycode; };
struct gpio_desc { unsigned int offset; bool row, active_low; };
static uint32_t physical_keys[MATRIX_MAX_COLS];
static bool columns[MATRIX_MAX_COLS];
static unsigned int reads, writes, syncs, schedules, delay_ms, fail_read, change_read;
static int irq_depth[MATRIX_MAX_ROWS];
static unsigned int event_count;
static struct { unsigned int code; bool down; } events[1024];
static void fake_guard(void *lock) { (void)lock; }
static void fsleep(unsigned int us) { (void)us; }
static void bitmap_to_arr32(u32 *dst, const unsigned long *src, unsigned int n)
{ *dst = (u32)src[0] & (n == 32 ? UINT32_MAX : (uint32_t)(BIT(n) - 1)); }
static int gpiod_direction_output(struct gpio_desc *g, int value)
{ assert(!g->row); columns[g->offset] = value; writes++; return 0; }
static void gpiod_set_value_cansleep(struct gpio_desc *g, int value)
{ assert(!g->row); columns[g->offset] = value; writes++; }
static int gpiod_direction_input(struct gpio_desc *g)
{ if (!g->row) columns[g->offset] = false; return 0; }
static int gpiod_get_array_value_cansleep(unsigned int n, struct gpio_desc **rows,
                                         void *info, unsigned long *values)
{
    uint32_t asserted = 0;
    (void)info;
    reads++;
    if (reads == change_read) physical_keys[0] ^= BIT(4);
    /* Garbage from a failed provider must never become input events. */
    if (reads == fail_read) { values[0] = UINT32_MAX; return -EIO; }
    for (unsigned int c = 0; c < MATRIX_MAX_COLS; c++)
        if (columns[c]) asserted |= physical_keys[c];
    values[0] = 0;
    for (unsigned int r = 0; r < n; r++) {
        bool raw = !!(asserted & BIT(rows[r]->offset)) ^ rows[r]->active_low;
        bool logical = raw ^ rows[r]->active_low;
        if (logical) values[0] |= BIT(r);
    }
    return 0;
}
static void enable_irq(unsigned int irq) { assert(irq_depth[irq] > 0); irq_depth[irq]--; }
static void disable_irq_nosync(unsigned int irq) { assert(irq_depth[irq] == 0); irq_depth[irq]++; }
static unsigned int msecs_to_jiffies(unsigned int ms) { return ms; }
static void schedule_delayed_work(struct delayed_work *work, unsigned int delay)
{ (void)work; schedules++; delay_ms = delay; }
static void input_event(struct input_dev *dev, int type, int code, int value)
{ (void)dev; (void)type; (void)code; (void)value; }
static void input_report_key(struct input_dev *dev, unsigned int code, int value)
{ (void)dev; assert(event_count < 1024); events[event_count].code = code;
  events[event_count++].down = !!value; }
static void input_sync(struct input_dev *dev) { (void)dev; syncs++; }
'''

HARNESS = r'''
static struct matrix_keypad keypad;
static struct input_dev input;
static struct gpio_desc rows[32], cols[32];
static unsigned short keycodes[1024];
static void reset_counts(void)
{ reads = writes = syncs = schedules = delay_ms = fail_read = change_read = event_count = 0; }
static void setup(bool poll)
{
    memset(&keypad, 0, sizeof(keypad));
    memset(physical_keys, 0, sizeof(physical_keys));
    memset(columns, 0, sizeof(columns));
    memset(irq_depth, 0, sizeof(irq_depth));
    reset_counts();
    for (unsigned int i = 0; i < 1024; i++) keycodes[i] = i;
    input.keycode = keycodes;
    keypad.input_dev = &input;
    keypad.num_row_gpios = 8;
    keypad.num_col_gpios = 7;
    keypad.row_shift = 3;
    keypad.poll_interval_ms = poll ? 20 : 0;
    keypad.drive_inactive_cols = true;
    keypad.scan_pending = true;
    for (unsigned int i = 0; i < 32; i++) {
        rows[i] = (struct gpio_desc){ .offset = i, .row = true, .active_low = true };
        cols[i] = (struct gpio_desc){ .offset = i, .row = false, .active_low = true };
        keypad.row_gpios[i] = &rows[i];
        keypad.col_gpios[i] = &cols[i];
        keypad.row_irqs[i] = i;
        columns[i] = i < 7;
        irq_depth[i] = poll ? 0 : 1;
    }
}
static void scan(void) { matrix_keypad_scan(&keypad.work.work); }
static void assert_active(void)
{ for (unsigned int c = 0; c < keypad.num_col_gpios; c++) assert(columns[c]); }
static void assert_state(void)
{ for (unsigned int c = 0; c < keypad.num_col_gpios; c++) assert(keypad.last_key_state[c] == physical_keys[c]); }
static void assert_event(unsigned int i, unsigned int row, unsigned int col, bool down)
{ assert(events[i].code == MATRIX_SCAN_CODE(row, col, keypad.row_shift)); assert(events[i].down == down); }
int main(void)
{
    setup(true);
    scan();
    assert(reads == 1 && writes == 0 && event_count == 0 && syncs == 0);
    assert(schedules == 1 && delay_ms == 20 && !keypad.scan_pending);
    assert_active();

    /* Two keys on different rows/columns, then hold and release both. */
    reset_counts(); physical_keys[2] = BIT(0); physical_keys[6] = BIT(7);
    scan(); assert(reads == 8 && event_count == 2);
    assert_event(0, 0, 2, true); assert_event(1, 7, 6, true);
    assert_state(); assert_active();
    reset_counts(); scan(); assert(reads == 8 && event_count == 0); assert_state();
    reset_counts(); memset(physical_keys, 0, sizeof(physical_keys));
    scan(); assert(reads == 8 && event_count == 2);
    assert_event(0, 0, 2, false); assert_event(1, 7, 6, false); assert_state();
    reset_counts(); scan(); assert(reads == 1 && writes == 0);

    /* Same-row chord loses one column while aggregate row stays asserted. */
    physical_keys[1] = BIT(3); physical_keys[4] = BIT(3); reset_counts(); scan();
    assert(event_count == 2); assert_state();
    physical_keys[1] = 0; reset_counts(); scan();
    assert(event_count == 1); assert_event(0, 3, 1, false); assert_state();

    /* Every failed read preserves the complete previous key state. */
    for (unsigned int failure = 1; failure <= 8; failure++) {
        setup(true); physical_keys[3] = BIT(5); scan();
        physical_keys[3] = 0; reset_counts(); fail_read = failure; scan();
        assert(event_count == 0 && syncs == 0 && keypad.last_key_state[3] == BIT(5));
        assert(schedules == 1 && delay_ms == 20 && !keypad.scan_pending); assert_active();
        reset_counts(); scan(); assert(event_count == 1); assert_event(0, 5, 3, false);
    }
    setup(true); keypad.stopped = true; scan(); assert(schedules == 0);
    setup(true); keypad.drive_inactive_cols = false;
    physical_keys[5] = BIT(6); scan(); assert_event(0, 6, 5, true); assert_active();
    reset_counts(); physical_keys[5] = 0; scan(); assert_event(0, 6, 5, false); assert_active();

    /* IRQ mode still scans, balances IRQ masks, and retries read errors. */
    setup(false); scan(); assert(reads == 9 && schedules == 0);
    for (unsigned int r = 0; r < 8; r++) assert(irq_depth[r] == 0);
    setup(false); keypad.debounce_ms = 5; change_read = 9; scan();
    assert(schedules == 1 && delay_ms == 5 && keypad.scan_pending);
    reset_counts(); scan(); assert_event(0, 4, 0, true); assert_state();
    assert(schedules == 0 && !keypad.scan_pending);
    for (unsigned int failure = 1; failure <= 9; failure++) {
        setup(false); physical_keys[0] = BIT(2); fail_read = failure; scan();
        assert(schedules == 1 && delay_ms == 1 && keypad.scan_pending);
        for (unsigned int r = 0; r < 8; r++) assert(irq_depth[r] == 1);
        if (failure <= 8) assert(event_count == 0 && keypad.last_key_state[0] == 0);
        assert_active();
        reset_counts(); scan(); assert_state();
        for (unsigned int r = 0; r < 8; r++) assert(irq_depth[r] == 0);
    }
    setup(false); keypad.stopped = true; fail_read = 1; scan();
    assert(schedules == 0 && !keypad.scan_pending);
    for (unsigned int r = 0; r < 8; r++) assert(irq_depth[r] == 0);

    /* Descriptor order and the highest supported row survive conversion. */
    setup(true); keypad.num_row_gpios = 32;
    keypad.row_gpios[0] = &rows[31]; keypad.row_gpios[31] = &rows[0];
    physical_keys[0] = BIT(0); u32 state = 0;
    assert(read_row_state(&keypad, &state) == 0 && state == BIT(31));
    state = 0x12345678; fail_read = reads + 1;
    assert(read_row_state(&keypad, &state) == -EIO && state == 0x12345678);
    reset_counts(); scan(); assert_event(0, 31, 0, true);
    reset_counts(); physical_keys[0] = 0; scan(); assert_event(0, 31, 0, false);
    puts("PASS: idle, press/hold/release, chords, all provider failures, IRQ transition/retry/stop, HiZ columns, row order/bit31");
    return 0;
}
'''

variants = {
    "production": production,
    "regression-full-idle-scan": production.replace("if (keypad->poll_interval_ms && !init_row_state)", "if (false && keypad->poll_interval_ms && !init_row_state)"),
    "regression-lost-release": production.replace("if (col == keypad->num_col_gpios)", "if (true)"),
    "regression-swallowed-read-error": production.replace("if (err)\n\t\treturn err;", "if (err)\n\t\terr = 0;"),
}
with tempfile.TemporaryDirectory(prefix="matrix-keypad-test-") as temp:
    for name, body in variants.items():
        path = Path(temp) / f"{name}.c"
        binary = path.with_suffix("")
        path.write_text(PRELUDE + body + HARNESS)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Wno-sign-compare", "-Werror", str(path), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], text=True, capture_output=True)
        if name == "production":
            if result.returncode:
                raise SystemExit(result.stderr)
            print(result.stdout.strip())
        else:
            assert result.returncode != 0, f"fixture missed {name}"
            print(f"PASS: rejected {name}")
