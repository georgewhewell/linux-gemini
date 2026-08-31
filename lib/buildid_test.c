// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/buildid.h>
#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#define BUILDID_NOTE_TYPE	3
#define BUILDID_TEST_BUF_SIZE	160

static const u8 buildid_test_gnu_name[] = "GNU";
static const u8 buildid_test_other_name[] = "Linux";
static const u8 buildid_test_id[BUILD_ID_SIZE_MAX] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
	0x11, 0x12, 0x13, 0x14,
};

static const u8 buildid_test_zeros[BUILD_ID_SIZE_MAX];

static size_t buildid_test_append_note(u8 *buf, size_t offset,
				       size_t capacity, u32 type,
				       const void *name, u32 namesz,
				       const void *desc, u32 descsz)
{
	Elf32_Nhdr nhdr = {
		.n_namesz = namesz,
		.n_descsz = descsz,
		.n_type = type,
	};
	size_t name_space = ALIGN((size_t)namesz, 4);
	size_t desc_space = ALIGN((size_t)descsz, 4);
	size_t note_size = sizeof(nhdr) + name_space + desc_space;

	if (offset > capacity || note_size > capacity - offset)
		return SIZE_MAX;

	memset(buf + offset, 0, note_size);
	memcpy(buf + offset, &nhdr, sizeof(nhdr));
	if (namesz)
		memcpy(buf + offset + sizeof(nhdr), name, namesz);
	if (descsz)
		memcpy(buf + offset + sizeof(nhdr) + name_space, desc, descsz);

	return offset + note_size;
}

static size_t buildid_test_append_gnu_note(u8 *buf, size_t offset,
					   size_t capacity,
					   const void *desc, u32 descsz)
{
	return buildid_test_append_note(buf, offset, capacity,
					BUILDID_NOTE_TYPE,
					buildid_test_gnu_name,
					sizeof(buildid_test_gnu_name),
					desc, descsz);
}

static void buildid_test_expect_rejected(struct kunit *test, const void *buf,
					 u32 buf_size, u32 expected_size,
					 const char *reason)
{
	u8 output[BUILD_ID_SIZE_MAX];
	int ret;

	memset(output, 0xa5, sizeof(output));
	ret = build_id_parse_buf_exact(buf, output, buf_size, expected_size);

	KUNIT_EXPECT_EQ_MSG(test, ret, -EINVAL, "%s", reason);
	KUNIT_EXPECT_MEMEQ_MSG(test, output, buildid_test_zeros, sizeof(output),
			       "%s", reason);
}

static void buildid_test_valid_with_neighbors(struct kunit *test)
{
	static const u8 other_desc[] = { 0xa1, 0xb2, 0xc3 };
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 output[BUILD_ID_SIZE_MAX];
	size_t size = 0;
	int ret;

	size = buildid_test_append_note(buf, size, sizeof(buf), 0x42,
					buildid_test_other_name,
					sizeof(buildid_test_other_name),
					other_desc, sizeof(other_desc));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	size = buildid_test_append_gnu_note(buf, size, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	size = buildid_test_append_note(buf, size, sizeof(buf), 0x43,
					buildid_test_other_name,
					sizeof(buildid_test_other_name),
					other_desc, sizeof(other_desc));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);

	memset(output, 0xa5, sizeof(output));
	ret = build_id_parse_buf_exact(buf, output, size,
				       BUILD_ID_SIZE_MAX);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, output, buildid_test_id, sizeof(output));
}

static void buildid_test_short_valid_zero_padded(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 output[BUILD_ID_SIZE_MAX];
	u8 expected[BUILD_ID_SIZE_MAX] = {};
	size_t size;
	int ret;

	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id, 7);
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	memcpy(expected, buildid_test_id, 7);
	memset(output, 0xa5, sizeof(output));

	ret = build_id_parse_buf_exact(buf, output, size, 7);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, output, expected, sizeof(output));
}

static void buildid_test_rejected_candidates(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 zero_id[BUILD_ID_SIZE_MAX] = {};
	size_t size;

	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	size = buildid_test_append_gnu_note(buf, size, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size, BUILD_ID_SIZE_MAX,
				     "duplicate GNU build ID");

	memset(buf, 0, sizeof(buf));
	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id,
					    BUILD_ID_SIZE_MAX - 1);
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size, BUILD_ID_SIZE_MAX,
				     "wrong build ID size");

	memset(buf, 0, sizeof(buf));
	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf), zero_id,
					    sizeof(zero_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size, BUILD_ID_SIZE_MAX,
				     "all-zero build ID");

	memset(buf, 0, sizeof(buf));
	size = buildid_test_append_note(buf, 0, sizeof(buf), 0x42,
					buildid_test_other_name,
					sizeof(buildid_test_other_name),
					buildid_test_id, 4);
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size, BUILD_ID_SIZE_MAX,
				     "missing GNU build ID");
}

static void buildid_test_truncated_notes(struct kunit *test)
{
	static const u8 short_name[] = { 'X', 'Y', '\0' };
	static const u8 short_desc[] = { 0x21, 0x22, 0x23 };
	Elf32_Nhdr nhdr = {
		.n_namesz = sizeof(buildid_test_gnu_name),
		.n_descsz = BUILD_ID_SIZE_MAX,
		.n_type = BUILDID_NOTE_TYPE,
	};
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	size_t size, valid_size;

	memcpy(buf, &nhdr, sizeof(nhdr));
	buildid_test_expect_rejected(test, buf, sizeof(nhdr) - 1,
				     BUILD_ID_SIZE_MAX, "truncated header");
	buildid_test_expect_rejected(test, buf, sizeof(nhdr) + 3,
				     BUILD_ID_SIZE_MAX, "truncated name");

	memset(buf, 0, sizeof(buf));
	valid_size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
						  buildid_test_id,
						  sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, valid_size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, valid_size - 1,
				     BUILD_ID_SIZE_MAX,
				     "truncated descriptor");

	size = buildid_test_append_note(buf, valid_size, sizeof(buf), 0x42,
					short_name, sizeof(short_name), NULL, 0);
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size - 1,
				     BUILD_ID_SIZE_MAX,
				     "truncated name padding");

	memset(buf + valid_size, 0, sizeof(buf) - valid_size);
	size = buildid_test_append_note(buf, valid_size, sizeof(buf), 0x42,
					buildid_test_gnu_name,
					sizeof(buildid_test_gnu_name),
					short_desc, sizeof(short_desc));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	buildid_test_expect_rejected(test, buf, size - 1,
				     BUILD_ID_SIZE_MAX,
				     "truncated descriptor padding");

	memset(buf + valid_size, 0, sizeof(buf) - valid_size);
	buf[valid_size] = 0xff;
	buildid_test_expect_rejected(test, buf, valid_size + 1,
				     BUILD_ID_SIZE_MAX, "trailing byte");
}

static void buildid_test_u32_size_overflow(struct kunit *test)
{
	static const struct {
		u32 namesz;
		u32 descsz;
		const char *reason;
	} cases[] = {
		{ U32_MAX, 0, "name alignment overflow" },
		{ 0, U32_MAX, "descriptor alignment overflow" },
		{ U32_MAX - 3, 0, "name total-size overflow" },
		{ 0, U32_MAX - 3, "descriptor total-size overflow" },
	};
	u8 buf[sizeof(Elf32_Nhdr)] = {};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		Elf32_Nhdr nhdr = {
			.n_namesz = cases[i].namesz,
			.n_descsz = cases[i].descsz,
			.n_type = BUILDID_NOTE_TYPE,
		};

		memcpy(buf, &nhdr, sizeof(nhdr));
		buildid_test_expect_rejected(test, buf, sizeof(buf),
					     BUILD_ID_SIZE_MAX,
					     cases[i].reason);
	}
}

static void buildid_test_unaligned_input(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 output[BUILD_ID_SIZE_MAX];
	size_t end;
	int ret;

	end = buildid_test_append_gnu_note(buf, 1, sizeof(buf),
					   buildid_test_id,
					   sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, end, SIZE_MAX);
	memset(output, 0xa5, sizeof(output));

	ret = build_id_parse_buf_exact(buf + 1, output, end - 1,
				       BUILD_ID_SIZE_MAX);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, output, buildid_test_id, sizeof(output));
}

static void buildid_test_invalid_arguments(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 output[BUILD_ID_SIZE_MAX];
	size_t size;
	int ret;

	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);

	buildid_test_expect_rejected(test, buf, size, 0,
				     "zero expected size");
	buildid_test_expect_rejected(test, buf, size, BUILD_ID_SIZE_MAX + 1,
				     "oversized expected size");
	buildid_test_expect_rejected(test, NULL, 0, BUILD_ID_SIZE_MAX,
				     "NULL input buffer");
	buildid_test_expect_rejected(test, buf, 0, BUILD_ID_SIZE_MAX,
				     "empty input buffer");

	memset(output, 0xa5, sizeof(output));
	ret = build_id_parse_buf_exact(buf, NULL, size, BUILD_ID_SIZE_MAX);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static void buildid_test_success_output_alias(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 *descriptor;
	size_t size;
	int ret;

	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	descriptor = buf + sizeof(Elf32_Nhdr) +
		     ALIGN(sizeof(buildid_test_gnu_name), 4);

	ret = build_id_parse_buf_exact(buf, descriptor, size,
				       BUILD_ID_SIZE_MAX);

	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, descriptor, buildid_test_id, BUILD_ID_SIZE_MAX);
}

static void buildid_test_failure_output_alias(struct kunit *test)
{
	u8 buf[BUILDID_TEST_BUF_SIZE] = {};
	u8 *descriptor;
	size_t size;
	int ret;

	size = buildid_test_append_gnu_note(buf, 0, sizeof(buf),
					    buildid_test_id,
					    sizeof(buildid_test_id));
	KUNIT_ASSERT_NE(test, size, SIZE_MAX);
	descriptor = buf + sizeof(Elf32_Nhdr) +
		     ALIGN(sizeof(buildid_test_gnu_name), 4);
	buf[size++] = 0xff;

	ret = build_id_parse_buf_exact(buf, descriptor, size,
				       BUILD_ID_SIZE_MAX);

	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, descriptor, buildid_test_zeros, BUILD_ID_SIZE_MAX);
}

static struct kunit_case buildid_test_cases[] = {
	KUNIT_CASE(buildid_test_valid_with_neighbors),
	KUNIT_CASE(buildid_test_short_valid_zero_padded),
	KUNIT_CASE(buildid_test_rejected_candidates),
	KUNIT_CASE(buildid_test_truncated_notes),
	KUNIT_CASE(buildid_test_u32_size_overflow),
	KUNIT_CASE(buildid_test_unaligned_input),
	KUNIT_CASE(buildid_test_invalid_arguments),
	KUNIT_CASE(buildid_test_success_output_alias),
	KUNIT_CASE(buildid_test_failure_output_alias),
	{}
};

static struct kunit_suite buildid_test_suite = {
	.name = "buildid-exact",
	.test_cases = buildid_test_cases,
};

kunit_test_suite(buildid_test_suite);

MODULE_DESCRIPTION("KUnit tests for strict GNU build ID note parsing");
MODULE_LICENSE("GPL");
