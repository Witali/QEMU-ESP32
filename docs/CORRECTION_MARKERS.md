# Function correction markers

`FIXED_EMULATION` in a function's opening comment identifies a verified model
behavior correction. Describe the wrong behavior and the invariant restored,
and link the reproducer and before/after evidence from the related module docs.
This lets future comparisons distinguish an intentional correction from an
unexplained behavioral difference. Ordinary refactoring needs its usual purpose
comment without inventing a model defect.

The first documented use is `esp32_wifi_learn_guest_ip` in
`hw/net/esp32c3_wifi_interface.c`. It restores the shared AP interface address
while retaining separate client leases. See [WIFI_AP_CLIENTS.md](WIFI_AP_CLIENTS.md)
and `validation/softap-stalled-peer-20261024.json` for the actual reproducer.
