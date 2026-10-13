/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD3_RUNTIME_H
#define _MT6797_MD3_RUNTIME_H

#define MD3_RUNTIME_SIZE 0x800U
#define MD3_QUERY_SIZE 0x64U
#define MD3_QUERY_FRAME_SIZE (MD3_QUERY_SIZE + 16U)

/* Fixed board addresses already independently admitted by the LK/MPU/remap
 * cold owner. This serializer cannot grant those resources or start firmware.
 * BOOT_INFO here is MD-query feature0; AP-query feature0 is AT_CHANNEL_NUM.
 */
static inline int md3_runtime_build(const u8 query[64], u8 *runtime,
				    size_t capacity, size_t *used,
				    u8 ap_query[MD3_QUERY_FRAME_SIZE], u32 sbp,
				    u32 c2k_mode, bool service_owned)
{
	u8 mask, version;
	u32 words[4], bytes;
	size_t pos = 0;
	unsigned int i, j;

	if (!query || !runtime || !used || !ap_query ||
	    capacity < MD3_RUNTIME_SIZE || !service_owned || c2k_mode != 2)
		return -EINVAL;
	/* Validate the whole query before writing any output. Unknown mandatory
	 * contracts and unknown versions must not become false success records.
	 */
	for (i = 0; i < 64; i++) {
		mask = query[i] & 15;
		version = query[i] >> 4;
		if (mask > 4)
			return -EPROTO;
		if (mask == 2 && (version || (i != 0 && i != 1 && i != 2 &&
					      i != 4 && i != 9 && i != 12)))
			return -EOPNOTSUPP;
	}
	memset(runtime, 0, MD3_RUNTIME_SIZE);
	for (i = 0; i < 64; i++) {
		mask = query[i] & 15;
		version = query[i] >> 4;
		bytes = 0;
		memset(words, 0, sizeof(words));
		/* Only the implemented mandatory service set is admitted. Optional
		 * SCP/logging/MEID/customer features receive NOT_SUPPORT explicitly.
		 */
		if (mask == 2) {
			switch (i) {
			case 0:
				bytes = 16;
				break; /* normal boot, idle logging, control0 */
			case 1:
				words[0] = 0x40400000;
				words[1] = 0x10000;
				bytes = 8;
				break;
			case 2:
				words[0] = 0x40419000;
				words[1] = MD3_RING_BYTES;
				bytes = 8;
				break;
			case 4:
				words[0] = 0x40200000;
				words[1] = 0x200000;
				bytes = 8;
				break;
			case 9:
				words[0] = sbp;
				bytes = 16;
				break; /* MD3 image type2 < UBIN8: WM0 */
			case 12:
				words[0] = BIT(0) | BIT(2);
				bytes = 16;
				break; /* owned MD3+SRLTE */
			}
		} else if (mask) {
			mask = 1;
			version = 0;
		}
		if (pos > MD3_RUNTIME_SIZE - 8 - bytes)
			return -EOVERFLOW;
		runtime[pos] = i;
		runtime[pos + 1] = mask | (version << 4);
		put_unaligned_le32(bytes, runtime + pos + 4);
		for (j = 0; j < bytes / 4; j++)
			put_unaligned_le32(words[j], runtime + pos + 8 + j * 4);
		pos += 8 + bytes;
	}
	memset(ap_query, 0, MD3_QUERY_FRAME_SIZE);
	put_unaligned_le32(MD3_QUERY_FRAME_SIZE, ap_query + 4);
	/* Active C2K sender uses tx_ch CONTROL_TX1 directly for SRAM, without
	 * ring-channel remapping or seq_num increment. Preserve that distinction.
	 */
	put_unaligned_le32(1, ap_query + 8);
	put_unaligned_le32(0x5555ffff, ap_query + 12);
	put_unaligned_le32(0x43434349, ap_query + 16);
	/* Query only the MD AT-channel capability, as the vendor does. This
	 * request does not grant an AP AT endpoint or make an AT readiness claim.
	 */
	ap_query[20] = 3;
	put_unaligned_le32(1, ap_query + 84); /* share_memory_support */
	put_unaligned_le32(0x40410000, ap_query + 88);
	put_unaligned_le32(0x800, ap_query + 92);
	put_unaligned_le32(0x40410800, ap_query + 96);
	put_unaligned_le32(0x800, ap_query + 100);
	put_unaligned_le32(0x40400000, ap_query + 104);
	put_unaligned_le32(0x200000, ap_query + 108);
	put_unaligned_le32(0x43434349, ap_query + 112);
	*used = pos;
	return 0;
}
#endif
