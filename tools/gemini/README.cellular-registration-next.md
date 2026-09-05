# Bounded registration and packet-data follow-up

This is a read-only source review, not permission to send commands or change
the current C1 transaction. C1 first has to establish real HS1 runtime enqueue,
HS2 and plain AT/OK under its already reviewed one-shot acceptance/recovery.
There is no current SIM/subscription result in this source review.

## Source-proven ports and limits

`mtk_mt6797_ccci.c` registers the standard WWAN AT port on UART2/queue1 and
calls `wwan_register_ops(..., 0)`. WWAN core attempts a default link with id0;
default link creation failure does not itself fail core registration, so
inspect the actual resulting netdev rather than assuming a name exists.

`mtk_ccci_net_channels()` maps transport id0 to RX20/TX22 on queue3. It accepts
ids0 through17 except8; queue3 serves ids0,1,7 and queue5 serves the others.
These are vendor CCMNI channel groups, not proof of an AT PDP CID mapping.
Do not silently assume AT CID1 corresponds to link0. TX accepts raw IPv4/IPv6,
prepends CCCI and counts successful enqueue; RX delivers validated framed
payloads. TX counters alone do not prove modem receipt or network delivery.

## Next useful bounded observations

After a successful C1 result and separate sole-owner authorization, query
each supported item once with a deadline: `AT+CPIN?`, `AT+CREG?`, `AT+CEREG?`,
`AT+CSQ`, `AT+CGATT?`, `AT+CGDCONT?`, `AT+CGACT?`. These inspect readiness,
registration/signal and existing packet contexts; unsupported commands must
remain explicit errors. No PIN attempts, operator-selection writes, APN
replacement, attach command, dial, call, SMS or subscription changes are
included. Avoid collecting IMSI/ICCID/IMEI when those identifiers are not
needed for the diagnosis.

Preserve actual SIM state, registration/reject information and configured
contexts before identifying a blocker. SIM absence or PIN demand needs the
user's physical/account input; no PIN guessing. A denial is not by itself
proof of an expired subscription. Missing APN settings need existing modem
configuration or user/carrier information rather than a guessed value.
Battery temperature is physically disconnected and is not a power blocker.

## Packet-data admission still to establish

Determine which existing context is associated with which CCMNI group and
what IP configuration the modem actually negotiated. A session activation,
new link, address/route change or traffic test is a separately reviewed step,
not hidden in these queries or C1 plain AT. Retain the existing SIM and
subscription. Use the actual observed context/addresses and a user-approved
endpoint for a tightly bounded exchange; save TX/RX counters and returned
payload, plus terminal-failure-free same-boot evidence. Interface presence,
carrier state, a transmitted packet, host/QEMU tests and HS2 alone are not
packet-service proof. Additional WWAN link creation may need an iproute2
userspace payload; do not assume the minimal C1 BusyBox supports that link type.
