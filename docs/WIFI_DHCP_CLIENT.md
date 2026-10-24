# Virtual AP client DHCP acquisition

The old model cast every data packet in the DHCP state to a fixed C struct,
read options beyond short packets and treated an OFFER as an established
lease. Option 53 had to be first. The new independent protocol module checks
SNAP, IPv4 IHL/length/fragmentation, UDP ports/length, BOOTP identity, cookie,
and bounded DHCP options. It handles PAD, END and option-overloaded file/sname
regions. Duplicate or malformed message/server identity options are rejected.
This is the virtual client's acquisition protocol, not a replacement guest stack.

Client MAC and transaction ID must match. A selected OFFER causes a REQUEST
with options 50 and 54. The peer remains in state 7 (requesting) until a matching
ACK from that server confirms the offered address; only then is state 5 ready.
A matching NAK clears the proposal and restarts discovery. Peer departure and
hardware reset clear all acquisition state. Late replies for another peer,
server, transaction or address do not change it.

References: [RFC 2131 section 4.4.1](https://www.rfc-editor.org/rfc/rfc2131.html#section-4.4.1)
and [RFC 2132 sections 9.3, 9.6, 9.7](https://www.rfc-editor.org/rfc/rfc2132.html#section-9.3).
Lease expiry/renewal/rebinding, fragmented-IP reassembly and RF/WPA remain
outside this acquisition model. Checksums are handled by the surrounding
network path; the parser does not claim full wire-protocol conformance.

## Verification, 2026-10-24

`tools/test_esp_wifi_dhcp.py --cc <gcc> --output <new-directory>` compiles the
actual C module and feeds independently serialized Python wire packets.
349 cases include option order/PAD, IHL 5/6/15, overload 0/1/2/3, every truncated
prefix, invalid envelopes/options and full offer/request/ACK/NAK isolation.
905 truncated reads end immediately at a Windows PAGE_NOACCESS boundary;
10,000 deterministic non-DHCP inputs are rejected. Failed parsing leaves the
output object unchanged; ignored transactions leave the lease unchanged.
Raw source/compiler-pinned report: validation/wifi-dhcp-unit-20261024.json.

Both complete target builds passed with warnings treated as errors. The
first integration build caught a local-variable shadow and was corrected;
its failed log is retained locally, not counted as success.
RISC-V SHA-256: `128d0d75316eeb2a33e7c527ce3cb71cbf9158b9e3062fe1a23c7bcccbf610f5`.
The existing yoRadio AP-peer harness passed in factory and reconstructed C2
images (1 -> 2 -> 1 -> 0 -> 1 peers, four successful HTTP reads each).
All owned C2 test VMs were stopped and reaped. Factory handle loss remains an
intentional firmware difference; this emulator change does not mask it.

C3 OLED default and modular profile each passed a 60-second boot/console/panic
and immutable-input check; reports: validation/wifi-dhcp-c3-20261024.json.
These are bounded boot checks, not complete application/network acceptance.
Xtensa executable remains byte-identical to the pre-peer baseline:
`821d98070aa131ccdc5c7ccd87336c90c13a7c205dfe991404ed242f9da0f0e1`.
Known earlier CYD IDF6 PHY acceptance limitations are not closed by that fact.
