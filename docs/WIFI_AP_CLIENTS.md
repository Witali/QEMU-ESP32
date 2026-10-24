# Independent virtual AP clients

`esp32c3-wifi` can model three independently associated virtual clients of the
guest AP. This is opt-in through the existing `softap-client-ssid` property.
The default presence mask is 1, preserving the previous single-client setup.

QMP QOM properties on `/machine/wifi`:

- `softap-client-mask`: bits 0..2 select present clients. Other bits are rejected
  before changing state or sending frames. Presence survives device reset;
  associations and DHCP state do not.
- `softap-clients`: read-only JSON string with slot, presence, packet-driven
  state, immutable client MAC and offered IPv4 address. These observations do
  not replace the guest SDK association/disconnection events.

The virtual MACs end in `02`, `03`, `04` under `52:54:00:43:00`.
Each uses its own probe/authentication/association and DHCP Client Identifier.
Radio, DMA and link shaping remain shared. Queued frames retain the exact
peer identity; departure discards only that peer's pending traffic and sends
an actual deauthentication management frame.

## Qualification checkpoint, 2026-10-24

Built both targets with DEBUG disabled on native Windows GCC 16.1.0.
RISC-V candidate SHA-256:
`288478784db0281c33b605a781d70c6b18c6ce5ef17667d21f8b04049127df00`.
Xtensa remained byte-identical to the pre-change executable:
`821d98070aa131ccdc5c7ccd87336c90c13a7c205dfe991404ed242f9da0f0e1`.

Small TV factory and reconstructed guest runs observed actual SDK events using
read-only hardware RSP: 1 -> 2 -> 1 -> 0 -> 1 associated clients, with separate
MACs. Ordinary HTTP succeeded at all four nonempty phases in both guests.
The reconstructed image started its HTTP server once, without any stop call.
The factory attempted three starts, lost its original handle on the second
start, and logged two `error in listen (112)` failures. All owned VMs were
stopped and reaped. The test and ABI extractor live in yoRadio under
`Small_TV/src/tests/network_clients_qemu.py` and `network_event_abi.py`.

This checkpoint covers departure of the second peer and return from zero.
Ethernet/SLIRP traffic still uses the primary virtual peer. Primary departure
while a secondary remains, per-client host endpoints, malformed DHCP handling,
ACK-before-ready, full APSTA traffic/churn and legacy board boot checks remain
separate gates. This is not complete network lifecycle or RF/WPA acceptance.
The QMP presence controls do not emulate radio physics or WPA encryption.

DHCP Client Identifier uniqueness follows
[RFC 2132 section 9.14](https://www.rfc-editor.org/rfc/rfc2132.html#section-9.14).


## Relay recovery checkpoint, 2026-10-24

The shared host Ethernet bridge now selects a present DHCP-bound peer instead
of always using slot 0. AP broadcasts use that same active relay. A normal ARP
request teaches the guest the changed L2 neighbor before forwarding IPv4 through
a new relay (or a different host source IP). It uses the existing radio/DMA path;
there are no guest RAM/ARP-table writes, direct HTTP calls or extra guest delays.
The existing link shaping/queue/loss policy applies to the ARP frame too.

The first prototype selected the surviving peer but still timed out: the guest
ARP entry pointed at the departed primary. The final probe targets the actual
AP IPv4 address and advertises the bridge host's source IP at the new peer MAC.
The SDK lwIP etharp_input updates that sender entry for a request directed to it.
This is one bridged host path; independent per-peer host backends/endpoints are
still a separate capability.

One factory reconnect also exposed missing management retransmission. The
virtual peer now distinguishes probe/authentication/association waits and retries
the outstanding exchange after one virtual second on the existing beacon event.
It does not restart an established association or add a guest busy wait.
Exhaustive management-loss injection and DHCP retransmission/lease renewal are
not claimed by this checkpoint.

Candidate SHA-256:
`cce6b4717d4da21ca1871b817ab697346d428d803f733377dac74ba6e57e5aaa`.
Factory and reconstructed images both passed departure of either peer and return
from zero (four runs, sixteen ordinary HTTP 200 responses). The native server
still starts once; the factory's repeated-start handle defect remains observable.
Failed primary-before-ARP controls and the stalled factory reconnect are retained
separately, not counted as passes. See validation/wifi-bridge-20261024.json and
the raw typed evidence in yoRadio at the stated path. Full lifecycle/traffic/churn,
RF/WPA and physical hardware acceptance remain open.


Final-candidate C3 OLED default/modular boot checks each passed for 60 seconds
(no panic/watchdog, correct console dimensions/content, unchanged inputs); raw
reports are included in validation/wifi-bridge-20261024.json. All owned processes
were stopped and reaped. Xtensa remains the unchanged baseline hash stated above;
no new claim about the known CYD IDF6 PHY issue is made.


## Bootstrap liveness with a stalled primary DHCP client

A repeated DISCOVER loss on peer 0 reproduced another bridge bug: peer 1 was
DHCP-bound and the native HTTP server was running, but the host HTTP request
remained pending through more than ten guest seconds. The request succeeded
only after permitting peer 0 to obtain a lease. The symmetric peer-1 stall
already worked. This was a real failing liveness test, not an expected-failure
success reclassified as recovery.

The model had learned the AP server's address only in peer 1's ipaddr, while
Ethernet/ARP demultiplexing consulted the primary slot. All clients belong to
one guest AP interface. esp32_wifi_learn_guest_ip now shares a learned AP address
across its peer slots, retaining separate offered_ipaddr leases and keeping STA
learning independent. AP address changes also update all these observations.
The corrected function carries FIXED_EMULATION; see CORRECTION_MARKERS.md.

Final candidate SHA-256: `a5be14c545fd2dffec145473429686b9a667735a8fb6f0226c5c60034ac3785f`.
Both stalled-peer cases now serve HTTP while the selected peer still has no
lease; clearing losses then completes its actual DHCP recovery. Additional
combined-loss runs each consumed all seven stage bits: probe/auth/assoc plus
Discover/Request/Offer/ACK. Both peer positions recovered, with four DHCP retries
and a healthy peer that served HTTP before the target completed DHCP.

The factory two-peer/primary-departure regression and C3 default/modular 60-second
boot/console/panic/input checks passed. Classic Xtensa is byte-identical to the
prior baseline. Raw numeric summaries are in
validation/softap-stalled-peer-20261024.json, with complete typed evidence in the
stated yoRadio path. Long churn/soak and RF/WPA/full lifecycle acceptance remain
open; the known earlier CYD IDF6 PHY issue is not closed by this checkpoint.
