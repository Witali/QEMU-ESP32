# Windows host forwarding and parallel browser requests

## Observed failure

A real Chrome page served by the Small TV guest starts six settings requests in
parallel. Some cold loads fail with `net::ERR_CONNECTION_REFUSED` before the
request reaches the guest. The UI then retains a default value: date format 0
instead of stored 2, or red/yellow/white instead of stored green/blue/yellow.
Successful requests and a clean UART alone do not establish correct page load.

The guest has six HTTP client sockets. A separate diagnostic also reproduces the
failure without an extra API probe. Guest packet capture has no corresponding
new-connection reset from the guest. These observations motivate checking the
host listener; they do not by themselves prove the cause.

## Dependency and candidate correction

Static Windows builds use pinned libslirp 4.9.3 commit
`dd76415fce457e319d665eb8210d05c5731360ba` through
`subprojects/slirp-win32.wrap`. The upstream `tcpx_listen()` initially requests
`listen(s, 1)`. The candidate patch requests `SOMAXCONN` on Windows, at the first
listen call. Other platforms retain the existing dependency and backlog.

Microsoft documents both queue-full `WSAECONNREFUSED` and the fact that another
listen call on a listening socket does not change its backlog:
[Winsock listen](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-listen).

`FIXED_EMULATION` marks this correction in the maintained dependency diff. It
means a deliberate emulator correction, not an attempt to reproduce an error
in the application firmware. Keep the explanation and source/specification
reference with the correction when updating the dependency.

The second dependency diff changes libslirp's static compiler argument from
`add_global_arguments` to `add_project_arguments`, allowing Meson to build it as
a subproject. Both the unpatched-listener control and candidate use that same
build adjustment. Do not compare different system libslirp versions to attribute
a result solely to the listener change.

## Browser regression

`tools/small_tv_browser_connection_test.py` launches its own disposable station
VM using an existing yoRadio Small TV build and the selected QEMU executable.
The Node worker uses real Chrome and the guest's actual HTML/JavaScript. Each
round loads a new browser context with an extra keep-alive probe, without a
probe, and with a Connection: close probe. Only that VM's loopback origin is
allowed. It verifies six groups of controls against actual stored API values,
all six successful GETs, and absence of browser errors.

The test records exact input hashes, raw browser events, Ethernet capture and
read-only socket snapshots decoded through the exact firmware ELF's DWARF.
The VM is stopped in cleanup; protected flash ranges, UART and one-ROM-boot are
checked. It neither patches guest code/RAM nor changes production firmware.

Example (supply local executable/module paths):

```text
python tools/small_tv_browser_connection_test.py --yoradio <yoRadio-worktree> --dependency-root <yoRadio-root> --qemu-root . --qemu <qemu-executable> --node <node> --playwright-modules <node_modules> --browser-executable <chrome> --rounds 20 --output <new-result-directory>
```

Run the identical harness and firmware with an unpatched-listener control built
using the same compiler, then the candidate. Retain failed control reports. A
successful narrow browser test does not replace the full Wi-Fi setup/cold-boot,
packet, existing-board or firmware regressions.

## Initial validation status (historical)

Investigation in progress. The previous installed QEMU reproduces one failed
load out of 18 in the new harness, with incorrect color controls and all input
hashes unchanged. Same-toolchain control/candidate comparison and network
regressions remain required. No claim of a verified repair is made here yet.


## Earlier listener-only checkpoint

The same-toolchain original listener failed 6 of 60 real page loads with
CONNECTION_REFUSED. The backlog candidate eliminated that error in this run,
but failed two loads with timeouts; it is **not accepted as a complete repair**.
A bounded poll diagnostic reproduced another timeout with only 9 poll sockets
against FD_SETSIZE 64 and no select error. An observer extension captured a
fourth timeout, including a proxy SYN_SENT entry. All failed reports are kept.

The exact 0592 image passed all three complete Wi-Fi setup/cold-start flows
(93 UI and 18 storage/role checks, 25 exact inputs) on candidate 2a3f. The packet
suite (12), shared NOR write groups (11 on three SoCs), module checks and both
C3 OLED boot profiles also pass. Full CYD compatibility remains unaccepted:
a 60 wall-second control showed no panic, while the candidate showed a cache
panic. The earlier PHY/cache baseline issues are not resolved by these results.

Fresh Meson fetched the pinned dependency, applied both wrap diffs and built it.
Its LF source matches the manual CRLF socket checkout after newline normalization.

## Separate TIME_WAIT collision

A captured host port 62291 was reused with a new initial sequence number.
The guest initially ACKed the old sequence; the proxy stayed SYN_SENT. The new
SYN finally received SYNACK after 41.671 **guest** seconds. Kernel TCP snapshots
show the host side already established during the failed request. The lwIP
`tcp_timewait_input` code ACKs out-of-window SYN and ignores RST in TIME_WAIT;
this is permitted TCP behavior, not grounds to shorten the firmware's timer.
See RFC 9293 section 3.6.1 and the tests' README.

A deterministic peer-port test now forces the actual guest LRU close, then
reuses the host source port. The backlog-only candidate times out in 3 seconds
with clean UART, one ROM boot, and unchanged inputs/protected flash. The first
prototype accidentally regenerated its setup image because persistent mode was
false; its startup failure is retained and excluded from the port-reuse claim.

The remaining correction must avoid reusing a guest-facing TCP tuple while it
can still be retained by the guest. The host and guest TCP port lifetimes must
be considered separately, using virtual time for guest-facing reservations.
Do not mask the collision with a delay, a longer browser timeout or a firmware
TIME_WAIT change. Port exhaustion, AP/STA isolation and migration behavior need
explicit treatment before accepting a generic proxy change.

## Guest-facing port leases

The third maintained dependency diff adds `src/hostfwd_port.c` and its header.
The proxy preserves the accepted host peer port when available. A live or
recently retired reservation instead selects a free guest-facing alias in
49152..65535; the accepted Windows socket is unchanged. Reservations are
conservative across AP/STA destinations and IPv4/IPv6 within one Slirp stack.
Other TCP tuples are checked too. Exhaustion rejects the new connection and
never steals an active reservation. The pool is bounded by 65535 ports.

A closed mapping remains reserved for 240000 ms plus upward rounding of at most
1 ms, using the guest virtual clock. This represents two RFC 9293 maximum
segment lifetimes. There is no host sleep, guest timer adjustment, firmware
TIME_WAIT shortcut, or larger browser timeout. The corrected call is marked
`FIXED_EMULATION` before the proxy constructs the first guest-facing SYN.

The local dependency uses migration state version 5, retaining the version-4
prefix and appending an HPF1 lease extension. It validates lengths, counts,
duplicates, ports, lifetimes and cursor before changing the pool. Remaining
lifetimes are rebased to the destination virtual clock, retired entries are
ordered by expiry, and existing live mappings are preserved. Legacy version-4
input remains readable but contains no prior lease history; that missing
history cannot be recovered. Unmodified version-4 receivers cannot read the
new format. `SLIRP_HOSTFWD_PORT_LEASES` identifies this local extension; it must
not be presented as the upstream library's unchanged migration format.
Live accepted host TCP streams are not migrated by this change. The backend
migration test does not establish complete ESP machine migration support.

## Final narrow validation, 2026-10-18

A causal control used the same current build configuration and source layout,
changing only the call to acquire a lease into a disabled `false &&` condition.
That executable timed out at 3.016 wall seconds in the deterministic peer-port
test. The enabled call returned HTTP 200 in 0.156 s. The final executable, after
the independent binary-file migration correction, returned HTTP 200 in 0.344 s.
All three retained the real firmware, clean UART, one ROM boot, protected flash
ranges and input hashes. Earlier controls used different generated configuration
bytes and are retained as historical evidence, not as a one-variable comparison.

The final RISC-V executable is
`d64f3013739f22b986e07aed39395b25bde474f52ef813b8ffaeedb0991edecc`;
Xtensa is `cd1163c34b8591ca5a2769703aadc317d724237abe9ca70e7700ba658c415ba3`.
Its package version string still says "yoRadio host-forwarding control";
labels are not build identities. Use the hashes and source/configuration
manifest. The disabled-call executable is a separate negative control.

The exact 0592 Small TV application passes all 60 real Chrome page loads and
all three Wi-Fi setup/cold-start flows (93 UI and 18 storage/role checks).
The library has 10 passing tests covering active leases, the quiet boundary,
ordinary tuples, IPv6, alias exhaustion, malformed/truncated migration input,
clock rebasing, live-pool merge, write failure and the public version-5 API
with legacy version-4 input. A fresh pinned wrap fetch applied all three diffs,
matched the manual sources after CRLF normalization, built, and passed the same
10 tests. Thin static archives are recorded together with the actual referenced
object files; the archive's own hash alone does not identify their contents.

Real QEMU file migration preserves the old peer reservation and selects a
separate alias after restoration. A separate all-byte RAM migration regression
also passes; its old-executable control fails. See
[the file transport regression](tests/README-migration-file.md).
The final packet suite (12), common NOR suite (11 groups, three SoCs), modular
SPI/I2C checks, host tools (11), and both unchanged C3 OLED profiles pass.
The first NOR attempt failed with ENOSPC and the first sandbox host-tool run
failed on access restrictions; both reports are retained separately from retries.

Full CYD acceptance remains open. In the new bounded 60-second comparison,
original 20B had no panic, while both the same-configuration disabled-call
control and final candidate showed PHY initialization and cache failures.
This does not attribute those failures to port leasing, and it does not prove
compatibility with the original installed CYD emulator. Preserve all controls
and investigate before merging this work into the default branch.

The earlier BC68 H10 lost its host workload and supervisor without a terminal
footer after 336 cycles. Its VM and UART guard remained alive; no additional
QMP/RSP client was attached. It is not a completed H10 or a firmware failure
claim. The original 31 runtime inputs and 11 snapshot inputs were rehashed
unchanged, as were all 539 inputs of the separate latest full run. A complete
H10 on the final firmware/emulator pair and full-suite acceptance are pending.
