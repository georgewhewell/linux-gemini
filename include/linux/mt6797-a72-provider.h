/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MT6797_A72_PROVIDER_H
#define __LINUX_MT6797_A72_PROVIDER_H

#include <linux/types.h>

/*
 * This is a private, platform-scoped callback boundary. It is not a
 * regulator consumer ABI and it does not authorize a hardware write by
 * itself. The provider owns the callback context and must attest every
 * returned field before the A72 owner can accept it.
 */
#define MT6797_A72_PROVIDER_CALL_ABI	1
#define MT6797_A72_PROVIDER_OPERATION_CPU8_UP	1
#define MT6797_A72_PROVIDER_CALL_SETTLE_US	1000
#define MT6797_A72_PROVIDER_CALL_DA921X_PAGE	0x80
#define MT6797_A72_PROVIDER_CALL_BUCKB_VSEL	0x46

struct mt6797_a72_provider_handle {
	u64 generation;
	u64 cookie;
};

struct mt6797_a72_provider_request {
	u32 abi;
	u32 operation;
	u32 settle_us;
	u32 da921x_page;
	u32 buckb_vsel;
	u32 reserved;
	u64 transaction_generation;
	u64 transaction_cookie;
};

struct mt6797_a72_provider_response {
	u32 abi;
	u32 returned;
	u32 vote_requested;
	u32 provider_mutated;
	u32 rail_mutated;
	u32 settle_us;
	u32 da921x_page;
	u32 buckb_enabled;
	u32 buckb_vsel;
	u32 origin;
	u32 reserved;
	u64 origin_generation;
	struct mt6797_a72_provider_handle held_handle;
};

struct mt6797_a72_provider_ops {
	int (*acquire)(void *context,
			const struct mt6797_a72_provider_request *request,
			struct mt6797_a72_provider_response *response);
	int (*release)(void *context,
			const struct mt6797_a72_provider_handle *handle,
			struct mt6797_a72_provider_response *response);
};

int mt6797_a72_provider_register(const struct mt6797_a72_provider_ops *ops,
				 void *context);
void mt6797_a72_provider_unregister(const struct mt6797_a72_provider_ops *ops,
				    void *context);
int mt6797_a72_provider_acquire(const struct mt6797_a72_provider_request *request,
					struct mt6797_a72_provider_response *response);
int mt6797_a72_provider_release(const struct mt6797_a72_provider_handle *handle,
					struct mt6797_a72_provider_response *response);

#endif /* __LINUX_MT6797_A72_PROVIDER_H */
