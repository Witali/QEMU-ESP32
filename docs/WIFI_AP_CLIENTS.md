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
