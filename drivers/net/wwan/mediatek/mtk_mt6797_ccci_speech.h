/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MTK_MT6797_CCCI_SPEECH_H
#define _MTK_MT6797_CCCI_SPEECH_H

/* Pure wire helpers. The caller supplies Linux types, errno and string APIs.
 * Contract: stock audio.primary.mt6797 ARM64 SHA256 d086eab26f9dbfa8...,
 * InitCcciMailbox e825c, SendMessage e8528, GetMessageID e82f8;
 * GPL vendor port_char.c supplies the final data-frame byte count.
 * No modem pointers, startup responses or calibration defaults are inferred.
 */
#define MT6797_SPEECH_RX 4U
#define MT6797_SPEECH_TX 5U
#define MT6797_SPEECH_HEADER 16U
/* The retained speech HAL allocates/reads one 0xd80-byte ccci_buff_t. */
#define MT6797_SPEECH_FRAME_MAX 0xd80U
#define MT6797_SPEECH_PARAMETERS_PREFIX 6U
#define MT6797_SPEECH_PARAMETERS_MAX \
	(MT6797_SPEECH_FRAME_MAX - MT6797_SPEECH_HEADER - \
	 MT6797_SPEECH_PARAMETERS_PREFIX)
#define MT6797_SPEECH_ON 0x2f20U
#define MT6797_SPEECH_OFF 0x2f21U
#define MT6797_SPEECH_DYNAMIC_PARAMETERS 0x2f79U
#define MT6797_SPEECH_RESET_REQUEST 0xaf70U

struct mt6797_speech_message {
	u16 id;
	u16 parameter;
	bool mailbox;
	const u8 *payload;
	size_t payload_len;
};

static inline u32 mt6797_speech_read32(const u8 *p)
{
	return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
		((u32)p[3] << 24);
}

static inline void mt6797_speech_write32(u8 *p, u32 value)
{
	p[0] = value;
	p[1] = value >> 8;
	p[2] = value >> 16;
	p[3] = value >> 24;
}

/* Called after common CCCI sequence/queue validation, before retaining RX.
 * Returned payload borrows the frame and expires when that frame is freed.
 */
static inline int mt6797_speech_parse(const u8 *frame, size_t bytes,
				     struct mt6797_speech_message *message)
{
	struct mt6797_speech_message decoded = { 0 };
	u32 first, packed;

	if (!frame || !message || bytes < MT6797_SPEECH_HEADER ||
	    bytes > MT6797_SPEECH_FRAME_MAX)
		return -EMSGSIZE;
	if ((mt6797_speech_read32(frame + 8) & 0xffff) != MT6797_SPEECH_RX)
		return -EPROTO;
	first = mt6797_speech_read32(frame);
	decoded.mailbox = first == 0xffffffffU;
	if (decoded.mailbox) {
		if (bytes != MT6797_SPEECH_HEADER)
			return -EPROTO;
		packed = mt6797_speech_read32(frame + 4);
	} else {
		if (bytes == MT6797_SPEECH_HEADER ||
		    mt6797_speech_read32(frame + 4) != bytes)
			return -EPROTO;
		/* data0 is an opaque transport field, never an AP memory address. */
		packed = mt6797_speech_read32(frame + 12);
		decoded.payload = frame + MT6797_SPEECH_HEADER;
		decoded.payload_len = bytes - MT6797_SPEECH_HEADER;
	}
	decoded.id = packed >> 16;
	decoded.parameter = packed & 0xffff;
	if ((decoded.id & 0xff00) != 0xaf00)
		return -EPROTO;
	*message = decoded;
	return 0;
}

/* No on/off command is sent by these helpers. The call owner supplies policy,
 * route lifetime, sequence and a real completion deadline. */
static inline int mt6797_speech_build_power(u8 *frame, size_t capacity,
					   bool on, u16 sequence)
{
	u16 id = on ? MT6797_SPEECH_ON : MT6797_SPEECH_OFF;

	if (!frame || capacity < MT6797_SPEECH_HEADER)
		return -EMSGSIZE;
	mt6797_speech_write32(frame, 0xffffffffU);
	mt6797_speech_write32(frame + 4, (u32)id << 16);
	mt6797_speech_write32(frame + 8, MT6797_SPEECH_TX |
		((u32)(sequence & 0x7fff) << 16) | 0x80000000U);
	mt6797_speech_write32(frame + 12, 0);
	return MT6797_SPEECH_HEADER;
}

/* Wrap actual board parameters; never substitute zeros or canned defaults.
 * The six-byte prefix is the retained SetVariousKindsOfSpeechParameters
 * e6f80 contract: LE u16 sync a2a2, type19, original parameter byte count.
 */
static inline int mt6797_speech_build_parameters(u8 *frame, size_t capacity,
						const void *parameters,
						size_t parameter_bytes,
						u16 sequence)
{
	size_t payload_bytes, total;

	if (!frame || !parameters || !parameter_bytes)
		return -EINVAL;
	if (parameter_bytes > MT6797_SPEECH_PARAMETERS_MAX)
		return -EMSGSIZE;
	payload_bytes = MT6797_SPEECH_PARAMETERS_PREFIX + parameter_bytes;
	total = MT6797_SPEECH_HEADER + payload_bytes;
	if (total > capacity)
		return -EMSGSIZE;
	/* Copy first so callers may pass a parameter slice within the output. */
	memmove(frame + MT6797_SPEECH_HEADER + MT6797_SPEECH_PARAMETERS_PREFIX,
		parameters, parameter_bytes);
	frame[16] = 0xa2;
	frame[17] = 0xa2;
	frame[18] = 19;
	frame[19] = 0;
	frame[20] = parameter_bytes;
	frame[21] = parameter_bytes >> 8;
	mt6797_speech_write32(frame, 0);
	mt6797_speech_write32(frame + 4, total);
	mt6797_speech_write32(frame + 8, MT6797_SPEECH_TX |
		((u32)(sequence & 0x7fff) << 16) | 0x80000000U);
	mt6797_speech_write32(frame + 12,
		((u32)MT6797_SPEECH_DYNAMIC_PARAMETERS << 16) | payload_bytes);
	return total;
}

/* JudgeAckOfMsg e8cf8's actual whitelist. AF70 is a request, not an ACK.
 * An owner must also enforce its call/reset generation before accepting this.
 */
static inline bool mt6797_speech_expects_ack(u16 waiting_id)
{
	u16 delta = waiting_id - 0x2f20U;
	bool expected = false;

	if (delta <= 0x24)
		expected = !!(0x1b3ff30227ULL & (1ULL << delta));
	delta = waiting_id - 0x2f71U;
	if (delta <= 0x10)
		expected |= !!(0x1814fU & (1U << delta));
	return expected;
}

static inline bool mt6797_speech_ack_matches(
		const struct mt6797_speech_message *message, u16 waiting_id)
{
	/* Stock GetMessageID and completion apply to both wire forms. Only a
 * successfully decoded, lifetime-pinned frame may reach this helper. */
	return message && mt6797_speech_expects_ack(waiting_id) &&
		(message->id & 0x8000) &&
		(message->id & 0x7fff) == waiting_id;
}

#endif
