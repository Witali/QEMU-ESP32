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
