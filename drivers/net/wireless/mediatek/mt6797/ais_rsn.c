// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pure RSN admission policy for the MT6797 GEN3 AIS station.
 *
 * GEN3 firmware carries no PMF or AKM state: vendor gen3 decides management
 * frame protection in the host (rsn.c rsnParseRsnIE/fgMgmtProtection) and only
 * programs keys and per-frame protection bits. This codec therefore decides,
 * from the exact RSN elements exchanged, whether an association uses PMF and
 * whether SAE authentication must be bridged to user space.
 */

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ais_rsn.h"

#define MT6797_WIFI_AIS_RSN_EID		48U
#define MT6797_WIFI_AIS_RSN_SUITE	4U
#define MT6797_WIFI_AIS_RSN_MAX_SUITES	16U
#define MT6797_WIFI_AIS_RSN_MAX_PMKIDS	8U

static bool mt6797_wifi_ais_rsn_take(const u8 **pos, const u8 *end, size_t length)
{
	if ((size_t)(end - *pos) < length)
		return false;
	*pos += length;
	return true;
}

static int mt6797_wifi_ais_rsn_list(const u8 **pos, const u8 *end, u16 *count,
				    const u8 **list, size_t size, unsigned int max)
{
	if (*pos == end)
		return 0;
	if (end - *pos < 2)
		return -EINVAL;
	*count = get_unaligned_le16(*pos);
	*pos += 2;
	if (!*count || *count > max)
		return -EINVAL;
	*list = *pos;
	return mt6797_wifi_ais_rsn_take(pos, end, (size_t)*count * size) ? 0 : -EINVAL;
}

int mt6797_wifi_ais_rsn_parse(const u8 *ie, struct mt6797_wifi_ais_rsn *rsn)
{
	const u8 *pos, *end;
	int ret;

	if (!rsn)
		return -EINVAL;
	memset(rsn, 0, sizeof(*rsn));
	if (!ie || ie[0] != MT6797_WIFI_AIS_RSN_EID || ie[1] < 2)
		return -EINVAL;
	pos = ie + 2;
	end = pos + ie[1];
	if (get_unaligned_le16(pos) != 1)
		return -EINVAL;
	pos += 2;
	rsn->group_cipher = MT6797_WIFI_AIS_RSN_CIPHER_CCMP;
	if (pos == end)
		return 0;
	rsn->group_cipher = get_unaligned_be32(pos);
	if (!mt6797_wifi_ais_rsn_take(&pos, end, MT6797_WIFI_AIS_RSN_SUITE))
		return -EINVAL;
	ret = mt6797_wifi_ais_rsn_list(&pos, end, &rsn->n_pairwise, &rsn->pairwise,
		MT6797_WIFI_AIS_RSN_SUITE, MT6797_WIFI_AIS_RSN_MAX_SUITES);
	if (ret)
		return ret;
	ret = mt6797_wifi_ais_rsn_list(&pos, end, &rsn->n_akm, &rsn->akm,
		MT6797_WIFI_AIS_RSN_SUITE, MT6797_WIFI_AIS_RSN_MAX_SUITES);
	if (ret)
		return ret;
	if (pos == end)
		return 0;
	if (end - pos < 2)
		return -EINVAL;
	rsn->capabilities = get_unaligned_le16(pos);
	pos += 2;
	if (pos == end)
		return 0;
	rsn->pmkid_offset = pos - ie;
	if (end - pos < 2)
		return -EINVAL;
	rsn->n_pmkid = get_unaligned_le16(pos);
	pos += 2;
	if (rsn->n_pmkid > MT6797_WIFI_AIS_RSN_MAX_PMKIDS)
		return -EINVAL;
	rsn->pmkid = rsn->n_pmkid ? pos : NULL;
	if (!mt6797_wifi_ais_rsn_take(&pos, end,
			(size_t)rsn->n_pmkid * MT6797_WIFI_AIS_RSN_PMKID_LENGTH))
		return -EINVAL;
	if (pos == end)
		return 0;
	rsn->group_mgmt_cipher = get_unaligned_be32(pos);
	rsn->has_group_mgmt_cipher = true;
	if (!mt6797_wifi_ais_rsn_take(&pos, end, MT6797_WIFI_AIS_RSN_SUITE))
		return -EINVAL;
	/* No later field is defined; refuse to forward bytes we did not parse. */
	return pos == end ? 0 : -EINVAL;
}

static bool mt6797_wifi_ais_rsn_has(const u8 *list, u16 count, u32 suite)
{
	u16 i;

	for (i = 0; i < count; i++)
		if (get_unaligned_be32(list + i * MT6797_WIFI_AIS_RSN_SUITE) == suite)
			return true;
	return false;
}

static bool mt6797_wifi_ais_rsn_akm_supported(u32 akm)
{
	return akm == MT6797_WIFI_AIS_RSN_AKM_PSK ||
	       akm == MT6797_WIFI_AIS_RSN_AKM_PSK_SHA256 ||
	       akm == MT6797_WIFI_AIS_RSN_AKM_SAE;
}

/*
 * PMF is in use exactly when both RSN elements set MFPC (IEEE 802.11-2020
 * 12.6.3). Only CCMP-128 pairwise/group and BIP-CMAC-128 are implemented.
 */
int mt6797_wifi_ais_rsn_policy(const struct mt6797_wifi_ais_rsn_request *request,
			       struct mt6797_wifi_ais_rsn_result *result)
{
	struct mt6797_wifi_ais_rsn sta, ap;
	bool sta_mfpc, sta_mfpr, ap_mfpc, ap_mfpr;
	int ret;

	if (result)
		memset(result, 0, sizeof(*result));
	if (!request || !result)
		return -EINVAL;
	ret = mt6797_wifi_ais_rsn_parse(request->sta_ie, &sta);
	if (ret)
		return ret;
	if (!mt6797_wifi_ais_rsn_akm_supported(request->akm) ||
	    sta.group_cipher != MT6797_WIFI_AIS_RSN_CIPHER_CCMP ||
	    sta.n_pairwise != 1 ||
	    get_unaligned_be32(sta.pairwise) != MT6797_WIFI_AIS_RSN_CIPHER_CCMP ||
	    sta.n_akm != 1 || get_unaligned_be32(sta.akm) != request->akm)
		return -EOPNOTSUPP;
	sta_mfpc = sta.capabilities & MT6797_WIFI_AIS_RSN_CAP_MFPC;
	sta_mfpr = sta.capabilities & MT6797_WIFI_AIS_RSN_CAP_MFPR;
	if (sta_mfpr && !sta_mfpc)
		return -EINVAL;
	if (sta.has_group_mgmt_cipher &&
	    (!sta_mfpc || sta.group_mgmt_cipher != MT6797_WIFI_AIS_RSN_CIPHER_BIP_CMAC))
		return -EOPNOTSUPP;

	/* Legacy WPA2-PSK without PMF keeps its accepted, AP-independent path. */
	if (!sta_mfpc && request->akm == MT6797_WIFI_AIS_RSN_AKM_PSK &&
	    !request->mfp_required && !request->sae_auth) {
		return sta.n_pmkid ? -EOPNOTSUPP : 0;
	}

	if (!request->ap_ie || mt6797_wifi_ais_rsn_parse(request->ap_ie, &ap) ||
	    !mt6797_wifi_ais_rsn_has(ap.akm, ap.n_akm, request->akm) ||
	    ap.group_cipher != MT6797_WIFI_AIS_RSN_CIPHER_CCMP ||
	    !mt6797_wifi_ais_rsn_has(ap.pairwise, ap.n_pairwise,
				     MT6797_WIFI_AIS_RSN_CIPHER_CCMP))
		return -EOPNOTSUPP;
	ap_mfpc = ap.capabilities & MT6797_WIFI_AIS_RSN_CAP_MFPC;
	ap_mfpr = ap.capabilities & MT6797_WIFI_AIS_RSN_CAP_MFPR;
	/* Either side requiring PMF that the other cannot provide is refused. */
	if ((sta_mfpr && !ap_mfpc) || (ap_mfpr && !sta_mfpc))
		return -EOPNOTSUPP;
	result->pmf = sta_mfpc && ap_mfpc;
	if (result->pmf && ap.has_group_mgmt_cipher &&
	    ap.group_mgmt_cipher != MT6797_WIFI_AIS_RSN_CIPHER_BIP_CMAC)
		return -EOPNOTSUPP;
	if (request->mfp_required && !result->pmf)
		return -EOPNOTSUPP;
	/* WPA3-Personal: SAE is only admitted together with PMF. */
	if (request->akm == MT6797_WIFI_AIS_RSN_AKM_SAE && !result->pmf)
		return -EOPNOTSUPP;
	if (request->sae_auth) {
		if (request->akm != MT6797_WIFI_AIS_RSN_AKM_SAE || !request->external_auth)
			return -EOPNOTSUPP;
		result->sae = true;
	} else if (request->akm == MT6797_WIFI_AIS_RSN_AKM_SAE && !sta.n_pmkid) {
		/* Open System with the SAE AKM is only PMKSA caching. */
		return -EOPNOTSUPP;
	}
	if (sta.n_pmkid && request->akm != MT6797_WIFI_AIS_RSN_AKM_SAE)
		return -EOPNOTSUPP;
	return 0;
}

int mt6797_wifi_ais_rsn_set_pmkid(u8 *ies, size_t *length, size_t capacity,
				  const u8 *pmkid)
{
	struct mt6797_wifi_ais_rsn rsn;
	u8 element[257];
	size_t offset, element_length, new_length, cut, tail, total;
	u8 *ie = NULL;

	if (!ies || !length || !pmkid || *length > capacity)
		return -EINVAL;
	for (offset = 0; offset < *length; offset += ies[offset + 1] + 2) {
		if (*length - offset < 2 || ies[offset + 1] > *length - offset - 2)
			return -EINVAL;
		if (ies[offset] != MT6797_WIFI_AIS_RSN_EID)
			continue;
		if (ie)
			return -EINVAL;
		ie = ies + offset;
	}
	if (!ie || mt6797_wifi_ais_rsn_parse(ie, &rsn) || !rsn.n_akm)
		return -EINVAL;
	element_length = ie[1] + 2;
	/* Capabilities are mandatory before a PMKID list. */
	cut = rsn.pmkid_offset;
	if (!cut) {
		cut = element_length;
		if (cut == (size_t)(rsn.akm - ie) + (size_t)rsn.n_akm * MT6797_WIFI_AIS_RSN_SUITE) {
			memcpy(element, ie, cut);
			element[cut] = 0;
			element[cut + 1] = 0;
			cut += 2;
		} else {
			memcpy(element, ie, cut);
		}
	} else {
		memcpy(element, ie, cut);
	}
	put_unaligned_le16(1, element + cut);
	memcpy(element + cut + 2, pmkid, MT6797_WIFI_AIS_RSN_PMKID_LENGTH);
	new_length = cut + 2 + MT6797_WIFI_AIS_RSN_PMKID_LENGTH;
	if (rsn.has_group_mgmt_cipher) {
		put_unaligned_be32(rsn.group_mgmt_cipher, element + new_length);
		new_length += MT6797_WIFI_AIS_RSN_SUITE;
	}
	if (new_length - 2 > 255)
		return -EMSGSIZE;
	element[1] = new_length - 2;
	offset = ie - ies;
	tail = *length - offset - element_length;
	total = *length - element_length + new_length;
	if (total > capacity)
		return -EMSGSIZE;
	memmove(ies + offset + new_length, ies + offset + element_length, tail);
	memcpy(ies + offset, element, new_length);
	*length = total;
	return 0;
}
