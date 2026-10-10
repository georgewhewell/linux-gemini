/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MT6797_WIFI_AIS_RSN_H
#define __MT6797_WIFI_AIS_RSN_H

#include <linux/types.h>

/* Suite selectors in cfg80211's host-order (OUI << 8 | type) encoding. */
#define MT6797_WIFI_AIS_RSN_CIPHER_CCMP		0x000fac04U
#define MT6797_WIFI_AIS_RSN_CIPHER_BIP_CMAC	0x000fac06U
#define MT6797_WIFI_AIS_RSN_AKM_PSK		0x000fac02U
#define MT6797_WIFI_AIS_RSN_AKM_PSK_SHA256	0x000fac06U
#define MT6797_WIFI_AIS_RSN_AKM_SAE		0x000fac08U
#define MT6797_WIFI_AIS_RSN_CAP_MFPR		0x0040U
#define MT6797_WIFI_AIS_RSN_CAP_MFPC		0x0080U
#define MT6797_WIFI_AIS_RSN_PMKID_LENGTH	16U

/* A bounded, borrowed view of one complete RSN element (ID and length included). */
struct mt6797_wifi_ais_rsn {
	const u8 *pairwise;
	const u8 *akm;
	const u8 *pmkid;
	size_t pmkid_offset;	/* Offset of the PMKID count field, or 0 if absent. */
	u32 group_cipher;
	u32 group_mgmt_cipher;
	u16 n_pairwise;
	u16 n_akm;
	u16 n_pmkid;
	u16 capabilities;
	bool has_group_mgmt_cipher;
};

struct mt6797_wifi_ais_rsn_request {
	const u8 *sta_ie;	/* The single RSN element user space will send. */
	const u8 *ap_ie;	/* The selected BSS's RSN element, or NULL. */
	u32 akm;		/* Station RSN selection, validated against allowed AKMs. */
	bool mfp_required;	/* NL80211_MFP_REQUIRED. */
	bool sae_auth;		/* NL80211_AUTHTYPE_SAE. */
	bool external_auth;	/* CONNECT_REQ_EXTERNAL_AUTH_SUPPORT. */
};

struct mt6797_wifi_ais_rsn_result {
	bool pmf;	/* Both peers advertise MFPC: robust management is protected. */
	bool sae;	/* Authenticate through the user-space SAE bridge. */
};

int mt6797_wifi_ais_rsn_parse(const u8 *ie, struct mt6797_wifi_ais_rsn *rsn);
int mt6797_wifi_ais_rsn_policy(const struct mt6797_wifi_ais_rsn_request *request,
			       struct mt6797_wifi_ais_rsn_result *result);
/*
 * Replace the PMKID list of the single RSN element inside @ies with @pmkid.
 * On failure the buffer and @length are unchanged.
 */
int mt6797_wifi_ais_rsn_set_pmkid(u8 *ies, size_t *length, size_t capacity,
				  const u8 *pmkid);

#endif /* __MT6797_WIFI_AIS_RSN_H */
