# Online A72 commit must not call PSCI AFFINITY_INFO

The MT6797 vendor implementation is not a read-only observer here. Public
ATF source `freedomtan/atf-1.0-x20` at 3fdcf7a6b3ca84185d4a985de98ce6a4d9c5f232,
services/std_svc/psci/psci_main.c:psci_affinity_info, calls power_off_big for a
present A72. plat/mt6797/power.c:power_off_big returns immediately only when
the private big_on bit is clear; otherwise it waits for target WFI and then
powers the target off. A running target querying itself can wait forever.
Moving that query to another CPU does not make it a valid online observer.

Ixoo's retained binary audit independently records this behavior in
experiments/2026-08-05-a72-secure-cpu-off-attribution/results/callgraph.tsv,
CG10, CG11 and CG15. Its audited binary SHA256 is
2cd154f332ee72edb6dee431a68eb5f8b98b4dc05ee14e56591cfbffcf81a9b3.
This is not an assertion that a differently packaged local firmware file
has that identity. The public-source and binary evidence both prohibit
using this call to observe an online A72.

The AP_ONLINE_DYN callback instead checks that it runs on the online target,
validates its actual r0p1 MIDR and the existing CPU4/CPU8 prerequisite, and
commits the already applied power transaction. It performs no firmware call.
The earlier cold admission checks remain before CPU_ON, with targets offline.
CPU9 still requires CPU8 online. Power-off and failed-start retry remain
unsupported. Actual usability requires subsequent runtime work on the CPU.

`test-a72-online-commit.py` compiles and executes the actual callback with
no PSCI/SMC interface and checks context, identity, prerequisite and failure
paths. This is a software regression test, not hardware qualification.
