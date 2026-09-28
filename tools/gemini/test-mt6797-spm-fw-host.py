#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for the MT6797 SPM PCM firmware parser and the SPM driver.

The production parser (drivers/soc/mediatek/mt6797-spm-fw.c) is compiled on
the host against small shims for the kernel headers it uses and run under
AddressSanitizer and UndefinedBehaviorSanitizer:

- synthetic images exercise every rejection rule and the exact vendor layout;
- every truncation and many random mutations of a valid image must be
  rejected or parsed without touching memory outside the buffer;
- the vendor per-CPU image selection is checked for all CPUs;
- with MT6797_SPM_FIRMWARE_DIR set, all nine vendor images must parse to
  the expected versions and sizes.

The driver is also checked to contain no SPM write accessor at all.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = root / 'drivers/soc/mediatek'
firmware_dir = os.environ.get('MT6797_SPM_FIRMWARE_DIR')
hostcc = os.environ.get('HOSTCC', 'cc')

driver = (src / 'mt6797-spm-drv.c').read_text()
for accessor in ('regmap_write', 'regmap_update_bits', 'regmap_set_bits',
                 'regmap_clear_bits', 'regmap_multi_reg_write',
                 'regmap_bulk_write', 'writel', 'iowrite32', 'ioremap',
                 'devm_platform_ioremap_resource'):
    if re.search(r'\b%s\w*\s*\(' % accessor, driver):
        raise SystemExit(f'driver must stay read-only; found {accessor}')

shims = {
    'linux/types.h': '''
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
''',
    # glibc's <errno.h> includes <linux/errno.h>; do not shadow it.
    'linux/errno.h': '#define EINVAL 22\n#define EFBIG 27\n',
    'linux/string.h': '#include <string.h>\n',
    'linux/unaligned.h': '''
#include <stdint.h>
static inline uint16_t get_unaligned_le16(const void *p)
{ const uint8_t *b = p; return b[0] | b[1] << 8; }
static inline uint32_t get_unaligned_le32(const void *p)
{ const uint8_t *b = p;
  return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
''',
}

expected = {
    'pcm_suspend.bin': (1822, 'pcm_suspend_v20'),
    'pcm_suspend_by_mp1.bin': (1822, 'pcm_suspend_v20_mp1'),
    'pcm_vcorefs_lpm.bin': (2037, 'pcm_vcorefs_lpm_v10p8_pmic_pwm_setting'),
    'pcm_vcorefs_hpm.bin': (1993, 'pcm_vcorefs_hpm_v10p8_pmic_pwm_setting'),
    'pcm_vcorefs_ultra.bin': (1993,
                              'pcm_vcorefs_ultra_v10p8_pmic_pwm_setting'),
    'pcm_sodi.bin': (1583, 'pcm_sodi_fpb_v15'),
    'pcm_sodi_by_mp1.bin': (1583, 'pcm_sodi_fpb_v15_mp1'),
    'pcm_deepidle.bin': (1657, 'pcm_deepidle_fpb_v17'),
    'pcm_deepidle_by_mp1.bin': (1657, 'pcm_deepidle_fpb_v17_mp1'),
}

vendor_table = '\n'.join(
    f'{{ "{name}", {words}, "{version}" }},'
    for name, (words, version) in expected.items())

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/errno.h>
#include "mt6797-spm-fw.h"

static u8 img[16384];

/* Build an image: words instructions, desc size, one vector, version. */
static size_t build(unsigned int words, unsigned int size, u32 vec0,
		    const char *version, unsigned int trailing)
{
	size_t off = 0, i;

	img[off++] = words & 0xff;
	img[off++] = words >> 8;
	for (i = 0; i < (size_t)words * 4; i++)
		img[off++] = (u8)(i * 7 + 3);
	img[off++] = size & 0xff;
	img[off++] = size >> 8;
	img[off++] = 2;			/* sess */
	img[off++] = 0;			/* replace */
	img[off++] = 0; img[off++] = 0;	/* addr_2nd */
	img[off++] = 0; img[off++] = 0;	/* reserved */
	for (i = 0; i < 16; i++) {
		u32 v = i ? 0 : vec0;

		img[off++] = v; img[off++] = v >> 8;
		img[off++] = v >> 16; img[off++] = v >> 24;
	}
	memcpy(img + off, version, strlen(version) + 1);
	off += strlen(version) + 1;
	memset(img + off, 'x', trailing);
	return off + trailing;
}

static int parse(size_t len)
{
	struct mt6797_spm_pcm pcm;
	/* exact-size heap copy so ASan catches any overread */
	u8 *copy = malloc(len ? len : 1);
	int ret;

	memcpy(copy, img, len);
	ret = mt6797_spm_pcm_parse(copy, len, &pcm);
	if (!ret) {
		assert(pcm.code == copy + 2);
		assert(pcm.size <= pcm.words);
		assert(strlen(pcm.version) < sizeof(pcm.version));
	}
	free(copy);
	return ret;
}

static void synthetic(void)
{
	struct mt6797_spm_pcm pcm;
	char ver[200];
	size_t len, cut;

	len = build(4, 4, (3u << 16) | 0x61, "v1", 0);
	assert(len == 2 + 16 + 72 + 3);
	assert(mt6797_spm_pcm_parse(img, len, &pcm) == 0);
	assert(pcm.words == 4 && pcm.size == 4 && pcm.sess == 2);
	assert(pcm.replace == 0 && pcm.addr_2nd == 0);
	assert(pcm.vec[0] == ((3u << 16) | 0x61) && pcm.vec[1] == 0);
	assert(!strcmp(pcm.version, "v1"));
	assert(pcm.code == img + 2 && pcm.code[0] == 3);

	for (cut = 0; cut < len; cut++)
		assert(parse(cut) != 0);

	assert(parse(build(0, 0, 0, "v", 0)) == -EFBIG);
	assert(parse(build(2049, 2049, 0, "v", 0)) == -EFBIG);
	assert(parse(build(2048, 2048, 0, "v", 0)) == 0);
	assert(parse(build(4, 5, 0, "v", 0)) == -EINVAL);
	assert(parse(build(4, 0, 0, "v", 0)) == -EINVAL);
	assert(parse(build(4, 3, 0, "v", 0)) == 0);
	assert(parse(build(4, 4, 4u << 16, "v", 0)) == -EINVAL);
	assert(parse(build(4, 4, (4u << 16) | 1, "v", 0)) == -EINVAL);
	assert(parse(build(4, 3, 3u << 16, "v", 0)) == -EINVAL);
	assert(parse(build(4, 4, 0, "", 0)) == -EINVAL);
	assert(parse(build(4, 4, 0, "v", 1)) == -EINVAL);

	len = build(4, 4, 0, "v", 0);
	img[len - 1] = 'x';		/* unterminated version */
	assert(parse(len) == -EINVAL);

	len = build(4, 4, 0, "v", 0);
	img[2 + 16 + 4] = 5;		/* addr_2nd > size */
	assert(parse(len) == -EINVAL);
	img[2 + 16 + 4] = 4;		/* addr_2nd == size */
	assert(parse(len) == 0);

	memset(ver, 'a', sizeof(ver));
	ver[127] = 0;
	assert(parse(build(4, 4, 0, ver, 0)) == 0);
	ver[127] = 'a';
	ver[128] = 0;
	assert(parse(build(4, 4, 0, ver, 0)) == -EINVAL);
}

static void fuzz(void)
{
	unsigned int seed = 1, round, flips;
	size_t len;

	for (round = 0; round < 200000; round++) {
		len = build(1 + round % 9, 1 + round % 9,
			    (u32)(round % 11) << 16, "fuzz", 0);
		for (flips = 0; flips < 1 + round % 4; flips++) {
			seed = seed * 1103515245 + 12345;
			img[(seed >> 8) % len] ^= (u8)(seed >> 3) | 1;
		}
		seed = seed * 1103515245 + 12345;
		parse((seed >> 4) % (len + 8));
	}
}

static void selection(void)
{
	unsigned int cpu;

	for (cpu = 0; cpu < 8; cpu++) {
		unsigned int mp1 = cpu >= 4;

		assert(mt6797_spm_fw_select(MT6797_SPM_SCENARIO_SUSPEND, cpu) ==
		       (int)(MT6797_SPM_FW_SUSPEND + mp1));
		assert(mt6797_spm_fw_select(MT6797_SPM_SCENARIO_SODI, cpu) ==
		       (int)(MT6797_SPM_FW_SODI + mp1));
		assert(mt6797_spm_fw_select(MT6797_SPM_SCENARIO_DEEPIDLE, cpu) ==
		       (int)(MT6797_SPM_FW_DEEPIDLE + mp1));
	}
	assert(mt6797_spm_fw_select(MT6797_SPM_SCENARIO_SUSPEND, 8) == -EINVAL);
	assert(mt6797_spm_fw_select(MT6797_SPM_SCENARIO_SODI, 9) == -EINVAL);
	assert(mt6797_spm_fw_select((enum mt6797_spm_scenario)7, 0) == -EINVAL);
	assert(!strcmp(mt6797_spm_fw_name[MT6797_SPM_FW_SODI_BY_MP1],
		       "mediatek/mt6797/pcm_sodi_by_mp1.bin"));
}

struct vendor { const char *name; unsigned int words; const char *version; };
static const struct vendor vendor[] = {
''' + vendor_table + r'''
};

static void vendor_images(const char *dir)
{
	unsigned int i, j, found;
	char path[4096];

	for (i = 0; i < sizeof(vendor) / sizeof(vendor[0]); i++) {
		struct mt6797_spm_pcm pcm;
		FILE *f;
		size_t len;
		u8 *buf;

		snprintf(path, sizeof(path), "%s/%s", dir, vendor[i].name);
		f = fopen(path, "rb");
		assert(f);
		buf = malloc(16384);
		len = fread(buf, 1, 16384, f);
		assert(feof(f));
		fclose(f);
		buf = realloc(buf, len);
		assert(mt6797_spm_pcm_parse(buf, len, &pcm) == 0);
		assert(pcm.words == vendor[i].words && pcm.size == pcm.words);
		assert(pcm.sess == 2 && pcm.replace == 0 && pcm.addr_2nd == 0);
		assert(!strcmp(pcm.version, vendor[i].version));
		found = 0;
		for (j = 0; j < MT6797_SPM_FW_COUNT; j++)
			if (!strcmp(strrchr(mt6797_spm_fw_name[j], '/') + 1,
				    vendor[i].name))
				found++;
		assert(found == 1);
		printf("vendor %s words=%u version=%s\n", vendor[i].name,
		       pcm.words, pcm.version);
		free(buf);
	}
}

int main(int argc, char **argv)
{
	synthetic();
	fuzz();
	selection();
	if (argc > 1)
		vendor_images(argv[1]);
	puts("mt6797-spm-fw host tests passed");
	return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, text in shims.items():
        path = tmp / 'include' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    (tmp / 'test.c').write_text(harness)
    exe = tmp / 'test'
    subprocess.run([hostcc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all',
                    '-I', str(tmp / 'include'), '-I', str(src),
                    str(src / 'mt6797-spm-fw.c'), str(tmp / 'test.c'),
                    '-o', str(exe)], check=True)
    args = [str(exe)]
    if firmware_dir:
        args.append(firmware_dir)
    subprocess.run(args, check=True)
    if not firmware_dir:
        print('vendor images skipped: MT6797_SPM_FIRMWARE_DIR is not set')
