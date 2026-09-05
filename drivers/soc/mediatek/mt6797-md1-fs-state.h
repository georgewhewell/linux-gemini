/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT6797_MD1_FS_STATE_H
#define _MT6797_MD1_FS_STATE_H

/* Every transition is protected by the provider lock. Cancellation is final. */
enum mt6797_fs_stage {
	MT6797_FS_EMPTY,
	MT6797_FS_INSTALLING,
	MT6797_FS_READY,
	MT6797_FS_CONSUMED,
	MT6797_FS_CANCELLED,
};

struct mt6797_fs_state {
	enum mt6797_fs_stage stage;
	bool reprobe_issued;
};

static int mt6797_fs_begin(struct mt6797_fs_state *s)
{
	if (s->stage != MT6797_FS_EMPTY)
		return -EALREADY;
	s->stage = MT6797_FS_INSTALLING;
	return 0;
}

static int mt6797_fs_publish(struct mt6797_fs_state *s)
{
	if (s->stage != MT6797_FS_INSTALLING)
		return -ECANCELED;
	s->stage = MT6797_FS_READY;
	return 0;
}

static int mt6797_fs_require(const struct mt6797_fs_state *s)
{
	if (s->stage == MT6797_FS_CONSUMED || s->stage == MT6797_FS_CANCELLED)
		return -EALREADY;
	return s->stage == MT6797_FS_READY ? 0 : -EPROBE_DEFER;
}

static int mt6797_fs_claim(struct mt6797_fs_state *s)
{
	int ret = mt6797_fs_require(s);

	if (!ret)
		s->stage = MT6797_FS_CONSUMED;
	return ret;
}

static int mt6797_fs_cancel(struct mt6797_fs_state *s)
{
	if (s->stage == MT6797_FS_CONSUMED)
		return -EBUSY;
	s->stage = MT6797_FS_CANCELLED;
	return 0;
}

static int mt6797_fs_reprobe(struct mt6797_fs_state *s)
{
	if (s->stage != MT6797_FS_READY || s->reprobe_issued)
		return -ECANCELED;
	s->reprobe_issued = true;
	return 0;
}

static int mt6797_fs_stats_convert(const struct kstatfs *in,
				    struct mt6797_md1_fs_stats *out)
{
	if (in->f_bsize < 512 || in->f_bsize > U32_MAX ||
	    in->f_bsize % 512 || !in->f_blocks || in->f_blocks > U32_MAX ||
	    in->f_bfree > in->f_blocks ||
	    (in->f_frsize && in->f_frsize != in->f_bsize))
		return -EOVERFLOW;
	*out = (struct mt6797_md1_fs_stats) {
		.block_size = in->f_bsize,
		.blocks = in->f_blocks,
		.free_blocks = in->f_bfree,
	};
	return 0;
}
#endif
