# C2 AP-MD1 shared-memory slice correction

Based on integration 40e3b806287c83567da52b7cb66689e64d28de9a.
Read-only evidence: handoff/cellular-c2-Dye0a5HI/dmesg-first-event.txt,
boot 2e265918-7126-496e-af40-4b376468d10a. No hardware access for this change.

The provider reports a 6 MiB parent CCCI reservation at 0x88000000, but its
RMPU region 7 is 0x88000000..0x881fffff: the 2 MiB AP-to-MD1 subdivision.
`mt6797_md1_lk_handoff_probe()` validates all live mblocks, parses the shared
layout, then publishes `ap_md_smem_size = smem.ap_md1_size`, not total_size.
The CLDMA admission constant incorrectly demanded total_size (6 MiB). Thus
the exact published valid layout is rejected with -ERANGE before CCCI maps
shared memory or acquires AP CLDMA registers.

Vendor provenance: gemini-vendor-kernel-3.18
8cfe6596a503612e3332d9c26e292a19525a7f07,
drivers/misc/mediatek/ccci_util/ccci_util_lib_fo.c:638-680.
Its smem_layout_t has independent AP-MD1, AP-MD3 and MD1-MD3 sizes/offsets;
share_memory_info_parsing() assigns MD_SYS1's size from ap_md1_smem_size and
address from base_addr + ap_md1_smem_offset. total_smem_size describes the
enclosing reservation, not MD_SYS1 ownership.

The correction accepts exactly the observed 2 MiB slice, preserving exact
base, MD image range, provider validation, GO/passive attestation and offset
underflow checks. CCCI consequently maps 0x88000000..0x881fffff only. No
reservation, MPU/remap, other modem subdivision or lifecycle authority expands.
The existing runtime advertisement remains 1 MiB. Its AP/MD runtime windows
are at 0x10000 with 0x800 bytes each, fitting within that advertised window
and the mapped 2 MiB slice; the only CCCI shared-memory write is the bounded
runtime reply at that offset. Other AP CLDMA admission predicates are not
changed by this correction and may still reject a later stage.

Tests use an independent literal 2 MiB fixture, rather than copying the
implementation's size constant (which hid the original error). Actual CCCI
handoff plus actual carveout validator accept the exact range and reject 27
failure cases, including both the whole 6 MiB reservation and a 1 MiB slice.
All refusals leave output/resource/runtime fields untouched. Actual common
header runtime-window constants are checked against the resulting mapping.
The in-kernel CLDMA KUnit fixture likewise uses 2 MiB and explicitly rejects
6 MiB. This is a source admission fix, not a successful modem-handshake claim.

Root integrated this correction as `8fd7f4c24d3ff`, independently reran the
27-refusal actual-code test, and compiled both CLDMA core and KUnit objects
for ARM64 successfully. The subsequent post-GO IP_BUSY admission audit remains
separate; the slice fix alone is not sufficient grounds to repeat C2.

C2 recovery closed at 04:16:34 with exact rescue, all three partition
readbacks, USB/RJ45 health and free locks. Recovered Stage A boot:
`e87053e9-3983-465b-b1da-c512cf38224d`. Its run-scoped pstore SHA256 is
`c1e0f9c01c8cfeeea79fa169b7abaa316f350c7a46bbdd35093f9b53a22049b1`,
attributable to C2 and its saved-evidence marker without an appended exception.
Candidate `336e4c0f4a867c5867fec4ba4af182d90e2d67cc27ffa39ad29e14512fd77045`
is consumed. No AT command or handshake success was observed.
