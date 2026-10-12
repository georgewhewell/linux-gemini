/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#include "../../drivers/net/wwan/mediatek/mtk_mt6797_ccci_speech.h"

static void rx_mailbox(u8 *p, u16 id, u16 parameter)
{
	/* Independent, bytewise fixture from the stock formatter wire layout. */
	memset(p, 0, 16);
	memset(p, 0xff, 4);
	p[4] = parameter;
	p[5] = parameter >> 8;
	p[6] = id;
	p[7] = id >> 8;
	p[8] = 4;
}

int main(int argc, char **argv)
{
	struct mt6797_speech_message message, unchanged;
	u8 *allocation = malloc(MT6797_SPEECH_FRAME_MAX + 3);
	u8 *frame = allocation + 1; /* Deliberately unaligned. */
	u8 small[16], untouched[16], parameters[7] = { 0, 2, 5, 0xff, 0xfe, 9, 13 };
	size_t i;
	int result;

	assert(allocation);
	memset(small, 0x59, sizeof(small));
	memcpy(untouched, small, sizeof(small));
	assert(mt6797_speech_build_power(small, 15, true, 0) == -EMSGSIZE);
	assert(!memcmp(small, untouched, sizeof(small)));
	assert(mt6797_speech_build_power(NULL, 16, true, 0) == -EMSGSIZE);
	assert(mt6797_speech_build_power(frame, 16, true, 0xffff) == 16);
	assert(frame[4] == 0 && frame[5] == 0 && frame[6] == 0x20 && frame[7] == 0x2f);
	assert(frame[8] == 5 && frame[9] == 0 && frame[10] == 0xff && frame[11] == 0xff);
	assert(mt6797_speech_build_power(frame, 16, false, 0x8000) == 16);
	assert(frame[6] == 0x21 && frame[10] == 0 && frame[11] == 0x80);

	/* Every truncation before the complete header, invalid channel and invalid
	 * speech ID must fail without modifying the decoded output. */
	memset(&message, 0x6a, sizeof(message));
	memcpy(&unchanged, &message, sizeof(message));
	rx_mailbox(frame, 0xaf20, 0x4433);
	for (i = 0; i < 16; ++i) {
		assert(mt6797_speech_parse(frame, i, &message) == -EMSGSIZE);
		assert(!memcmp(&message, &unchanged, sizeof(message)));
	}
	assert(mt6797_speech_parse(frame, 16, &message) == 0);
	assert(message.mailbox && message.id == 0xaf20 && message.parameter == 0x4433);
	assert(!message.payload && message.payload_len == 0);
	assert(mt6797_speech_ack_matches(&message, 0x2f20));
	assert(!mt6797_speech_ack_matches(&message, 0x2f21));
	assert(!mt6797_speech_ack_matches(&message, 0));
	frame[8] = 5;
	assert(mt6797_speech_parse(frame, 16, &message) == -EPROTO);
	frame[8] = 4;
	frame[7] = 0x2f;
	assert(mt6797_speech_parse(frame, 16, &message) == -EPROTO);
	rx_mailbox(frame, 0xaf70, 0);
	assert(mt6797_speech_parse(frame, 16, &message) == 0);
	assert(!mt6797_speech_ack_matches(&message, 0x2f70));
	assert(!mt6797_speech_ack_matches(&message, 0x2f20));
	assert(mt6797_speech_parse(frame, 17, &message) == -EPROTO);

	/* Only stock-listed acknowledgements may complete a waiter; requests
 * and unknown IDs never do. Stock accepts either decoded wire form. */
	for (i = 0; i <= 0xffff; ++i) {
		message.id = i;
		message.mailbox = true;
		if (mt6797_speech_ack_matches(&message, i & 0x7fff))
			assert((i & 0xff00) == 0xaf00 && i != 0xaf70);
	}
	/* These four IDs changed between the older Gemian and current Android
	 * HAL. The exact-current isolated oracle checks every ID below. */
	for (i = 0x2f34; i <= 0x2f37; ++i) {
		message.id = i | 0x8000;
		assert(!mt6797_speech_expects_ack(i));
		assert(!mt6797_speech_ack_matches(&message, i));
	}
	rx_mailbox(frame, 0xaf79, 0);
	assert(mt6797_speech_parse(frame, 16, &message) == 0);
	assert(mt6797_speech_ack_matches(&message, 0x2f79));
	message.mailbox = false;
	assert(mt6797_speech_ack_matches(&message, 0x2f79));

	/* Parameter wrapper golden bytes come from the exact stock stores:
	 * sync a2a2, type19, u16 original byte count; data-message ID in reserved.
	 * No output change on null, empty, oversized or insufficient-capacity data. */
	assert(mt6797_speech_build_parameters(small, sizeof(small), parameters, 7, 0) == -EMSGSIZE);
	assert(!memcmp(small, untouched, sizeof(small)));
	assert(mt6797_speech_build_parameters(small, sizeof(small), NULL, 7, 0) == -EINVAL);
	assert(mt6797_speech_build_parameters(small, sizeof(small), parameters, 0, 0) == -EINVAL);
	assert(mt6797_speech_build_parameters(small, sizeof(small), parameters,
		MT6797_SPEECH_PARAMETERS_MAX + 1, 0) == -EMSGSIZE);
	assert(!memcmp(small, untouched, sizeof(small)));
	result = mt6797_speech_build_parameters(frame, MT6797_SPEECH_FRAME_MAX, parameters, 7, 1);
	assert(result == 29);
	assert(!memcmp(frame + 16, (u8[]){ 0xa2, 0xa2, 19, 0, 7, 0 }, 6));
	assert(!memcmp(frame + 22, parameters, 7));
	assert(!memcmp(frame, (u8[]){ 0, 0, 0, 0, 29, 0, 0, 0, 5, 0, 1, 0x80,
		13, 0, 0x79, 0x2f }, 16));
	/* Turn this exact frame into an RX data message, preserving its in-band
	 * bytes and deliberately providing an opaque nonzero data0 value. */
	frame[0] = 0x23;
	frame[1] = 0x17;
	frame[8] = 4;
	frame[15] = 0xaf;
	assert(mt6797_speech_parse(frame, result, &message) == 0);
	assert(!message.mailbox && message.id == 0xaf79);
	assert(message.payload == frame + 16 && message.payload_len == 13);
	assert(mt6797_speech_ack_matches(&message, 0x2f79));
	frame[4] = 28;
	assert(mt6797_speech_parse(frame, result, &message) == -EPROTO);
	assert(mt6797_speech_parse(frame, MT6797_SPEECH_FRAME_MAX + 1, &message) == -EMSGSIZE);
	assert(mt6797_speech_parse(NULL, 16, &message) == -EMSGSIZE);
	assert(mt6797_speech_parse(frame, 16, NULL) == -EMSGSIZE);

	/* Aliased parameter input and the exact upper bound must remain intact. */
	memcpy(frame, parameters, 7);
	assert(mt6797_speech_build_parameters(frame, MT6797_SPEECH_FRAME_MAX, frame, 7, 2) == 29);
	assert(!memcmp(frame + 22, parameters, 7));
	memset(frame, 0x57, MT6797_SPEECH_PARAMETERS_MAX);
	assert(mt6797_speech_build_parameters(frame, MT6797_SPEECH_FRAME_MAX, frame,
		MT6797_SPEECH_PARAMETERS_MAX, 2) == MT6797_SPEECH_FRAME_MAX);
	for (i = 22; i < MT6797_SPEECH_FRAME_MAX; ++i)
		assert(frame[i] == 0x57);

	/* Optional stock oracle comparison excludes sequence bits supplied by
	 * the common native CCCI transport, and compares all other power bytes. */
	if (argc >= 2) {
		FILE *input = fopen(argv[1], "rb");
		u8 stock[32];
		assert(input && fread(stock, 1, sizeof(stock), input) == sizeof(stock));
		assert(fgetc(input) == EOF);
		fclose(input);
		for (i = 0; i < 2; ++i) {
			assert(mt6797_speech_build_power(frame, 16, i == 0, 0) == 16);
			frame[11] &= 0x7f;
			assert(!memcmp(frame, stock + i * 16, 16));
		}
	}
	if (argc == 3) {
		FILE *input = fopen(argv[2], "rb");
		u8 stock[256];
		assert(input && fread(stock, 1, sizeof(stock), input) == sizeof(stock));
		assert(fgetc(input) == EOF);
		fclose(input);
		message.mailbox = true;
		for (i = 0; i < 256; ++i) {
			message.id = 0xaf00 + i;
			assert(mt6797_speech_ack_matches(&message, 0x2f00 + i) == !!stock[i]);
		}
	}
	free(allocation);
	puts("PASS: speech codec malformed/unaligned/bounded frames, parameters and ACK ownership");
	if (argc >= 2) puts("PASS: supplied stock formatter oracle");
	if (argc == 3) puts("PASS: supplied exhaustive 256-ID stock ACK oracle");
	return 0;
}
