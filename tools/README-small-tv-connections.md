# Small TV host-connection regressions

These tests execute the archived, unchanged reconstructed firmware in disposable
headless QEMU VMs. They use loopback only and never flash a physical board.
A failed report must remain failed even when a narrower Wi-Fi test passes.

## Parallel browser settings

`small_tv_browser_connection_test.py` uses real Chrome and the guest HTML/JS.
Each round opens a fresh context with an extra keep-alive probe, without a
probe, and with a Connection: close probe. All six settings GETs must complete,
all six groups of controls must match the actual API values, and browser errors
or timeouts fail the load. The test captures Ethernet, read-only ELF/DWARF socket
snapshots and the host proxy's `info usernet` table. Input hashes and protected
flash ranges are checked during cleanup.

The yoRadio worktree must contain its matching archived application and native
ELF. The dependency root supplies the genuine C2 ROM ELF and oracle Python
packages. Supply the installed Node, Playwright module directory and Chrome:

```text
python tools/small_tv_browser_connection_test.py --yoradio <built-yoRadio-worktree> --dependency-root <yoRadio-root> --qemu-root . --node <node.exe> --playwright-modules <node_modules> --browser-executable <chrome.exe> --rounds 20 --output <new-result-directory>
```

## Reuse after a guest-initiated close

`small_tv_peer_port_test.py` needs only Python and QEMU. It first creates eight
real idle HTTP sessions. The native server's six-session limit makes its own LRU
close the oldest session. After observing EOF, the test rebinds that same local
host port and sends another request. Success requires a real guest response
within three seconds. No guest TCP state, timer, code or RAM is patched.

```text
python tools/small_tv_peer_port_test.py --yoradio <built-yoRadio-worktree> --dependency-root <yoRadio-root> --qemu-root . --output <new-result-directory>
```

The test retains the actual host port, LRU EOF, response timing, proxy state,
Ethernet capture, UART and input/flash guards. A current negative control
reproduces a host-established connection that remains `SYN_SENT` in the proxy
while the guest holds the previous tuple in TIME_WAIT. This deterministic case
must pass after a repair; extending the timeout does not repair the collision.

TCP peers may retain TIME_WAIT for 2 MSL. Early reopening is optional and has
additional conditions; do not require the firmware to discard that protection:
[RFC 9293 section 3.6.1](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.6.1).

PCAP timestamps are QEMU virtual time plus the capture's start timestamp, not a
wall-time duration. `observed_utc` is a separate host observation timestamp.
Neither test establishes full board, RF, long-duration or firmware acceptance.
