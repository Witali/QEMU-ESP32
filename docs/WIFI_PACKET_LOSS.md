# Exact one-shot loss at virtual AP peer boundaries

QMP `/machine/wifi` properties:

- `softap-drop-next` is a 21-bit mask, initially zero. Each peer owns seven
  consecutive bits: probe request, authentication request, association request,
  DHCP DISCOVER, REQUEST, OFFER and ACK. A matching packet consumes one bit.
- `softap-client-faults` is a read-only JSON string with per-peer stage counters
  and the last loss virtual time in microseconds. No packet payload is stored.

Fault policy and counters belong to the external test harness, so guest hardware
reset does not clear them. Setting a mask never changes guest RAM, code, PC,
registers or SDK events. Unknown bits are rejected before mutation. Zero exits
before packet parsing or clock access, and STA traffic is excluded.

Model-owned requests are classified using their constructor's bounded format;
DHCP server replies use the normal bounded parser. Drops occur at the virtual
client protocol boundary, after normal guest radio TX completion. They do not
claim to emulate physical RF loss, 802.11 acknowledgements or WPA encryption.
The existing randomized link loss remains a separate control.

## Before-retry qualification, 2026-10-24

Candidate SHA-256: `51b6746191d5dc910487ab1540b4279979f50a30eeec5da7fa74054f0674fb1a`.
The yoRadio `Small_TV/src/tests/network_loss_qemu.py` harness observed actual SDK
associations of two peers, armed a loss for the second peer, required exactly
one consumed counter and checked the unaffected first peer plus HTTP 200.
All seven owned VMs were stopped and reaped. Invalid masks were rejected.

The existing management retransmission recovered probe/auth/association loss.
DISCOVER/REQUEST/OFFER/ACK loss reproduced a missing DHCP retry bug: after more
than twelve guest seconds the target remained in state 6/7, while the first
peer and native HTTP server continued working. Those four are successful
**before controls**, with recovery_acceptance=false, not recovery qualification.
See validation/wifi-packet-loss-before-20261024.json. The next change must add
DHCP timeout/backoff and repeat these cases; complete lifecycle is still open.
The classic Xtensa executable is unchanged; final C3 regression remains required.


## DHCP retry correction

The following change fixes all four reproduced acquisition stalls. Each virtual
peer now schedules a deadline and retransmits DISCOVER or the selected-server
REQUEST. Nominal intervals are 4, 8, 16, 32, 64 seconds with uniform millisecond
jitter in -1..+1 seconds, capped at 64 s. The existing guest-AP beacon event
checks deadlines; no guest delay or host polling loop is inserted. ACK cancels
the deadline, NAK starts a new acquisition, departure/reset clear acquisition
state. Late or duplicate replies do not reset an unrelated transaction.

The previous common constant transaction ID was also replaced with fresh random
per-peer IDs on acquisition. IDs remain unchanged during retransmission; the
parser matches the specific lease ID. Constructors use BOOTREQUEST and the same
MAC in chaddr/client identifier, including the legacy virtual client mode.
Readonly client observations include XID, retry count and current deadline.
This implements initial acquisition retries, not lease renewal/rebinding.

Reference: [RFC 2131 section 4.1](https://www.rfc-editor.org/rfc/rfc2131.html#section-4.1).
Rejection sampling supplies uniform jitter rather than biased modulo sampling.

Final RISC-V SHA-256:
`6d9d82665330a6f567d04eac9563ed05d2d14bdb47329d78e9f306fce3761a90`.
All seven single-packet loss cases recovered with one consumed fault. Each DHCP
case performed exactly one retry, the healthy peer zero, both IDs were distinct
and stable. The pure C protocol test passed 351 cases, 905 guard-page prefixes,
10000 non-DHCP inputs and all 14007 backoff/jitter combinations. Raw reports:
validation/wifi-dhcp-retry-20261024.json and wifi-dhcp-retry-unit-20261024.json.

Factory two-peer/primary-departure/zero->survivor regression passed; its known
HTTP handle defect remains observable. C3 default/modular each passed a 60-second
boot/console/panic/immutable-input check. Classic Xtensa remains byte-identical
`821d98070aa131ccdc5c7ccd87336c90c13a7c205dfe991404ed242f9da0f0e1`.
No full CYD PHY, prolonged churn, all-loss combinations or RF/WPA claim is made.
