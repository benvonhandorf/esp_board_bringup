# WiFi

[← Command reference](README.md) · [Project README](../README.md)

This menu allows the user to perform actions on the wifi subsystem.

The radio is either a station or an access point, never both: a single radio
cannot sit on two channels, and an AP+station setup would silently drag the AP
onto whatever channel the station associated on. `connect` therefore stops a
running AP and `ap` drops a station association, each saying so as it happens.

## `scan`

Scans nearby wifi APs and outputs each AP including the SSID, RSSI, channel,
security and BSSID.

Scanning is a station-mode operation, so it is refused while an access point is
running; stop it with `wifi ap stop` first.

## `connect <AP> [password] [channel] [bssid]`

Connects to the specified access point.  Connection status and IP address are reported to the user, including any disconnections or changes in the future.
Disconnect reasons are decoded to readable text. Credentials are stored in NVS,
so `autostart` rejoins the same network after a reset. Omit the password for an
open network. A running access point is stopped first.

The channel and BSSID are both optional. Giving the channel hints the driver
to probe it before falling back to a full scan, connecting faster and more
reliably. Giving the BSSID (six colon-separated hex octets, copy-pasteable
straight from `scan` output, e.g. `aa:bb:cc:dd:ee:ff`) pins the association
to one specific AP -- useful when several APs advertise the same SSID, as on
a mesh or enterprise network. The SSID is still required even when a BSSID
is given, since it's part of the WPA key derivation. Because both are
positional, a BSSID without a channel hint is spelled with an empty channel:
`wifi connect home mypassword "" aa:bb:cc:dd:ee:ff`.

## `ap <SSID> [password] [channel]`

Hosts an access point so a laptop or phone can join the board directly and reach
the web interface with no network in the middle — which is the point on a bench
where there is no usable AP to join.

The password is optional; omit it for an open network. WPA2 requires at least 8
characters, and the SSID is limited to 32. The channel defaults to 1 and must be
1–13. Because the password is positional, an open network on a specific channel
is spelled with an empty password: `wifi ap bench "" 6`.

Clients get addresses over DHCP from the device, which is `192.168.4.1`. The web
console starts as soon as the AP is up — unlike station mode there is no lease to
wait for — so `http://192.168.4.1/` is live immediately. Clients joining and
leaving are reported as they happen, with their MAC address and AID.

## `ap stop`

Stops the access point and returns the radio to station mode. The web server
stays bound and becomes reachable again as soon as a station association
provides an address.

Note that stopping the AP disconnects any client using it — including the
browser you typed the command into, if you reached the web console that way.

## `autostart`

Joins the network stored in NVS, or hosts an access point when there is none.

This runs automatically at boot, so a board is reachable over the web interface
without a serial session at all. A board that has been through `wifi connect`
comes back on the same network after a reset; a fresh one, or one whose network
is out of range, raises its own access point instead:

- SSID `esp-bringup-<xxxxxx>`, where the suffix is the low three bytes of the
  board's soft AP MAC address, so several boards on a bench stay distinct
- password `bringup1234` (compiled in, so it is a speed bump rather than
  security — but better than an open console on a shared bench)
- channel 1, address `192.168.4.1`

The stored-network attempt uses a 10 second timeout rather than the 20 seconds
`connect` allows, so a board that cannot reach its network starts hosting
promptly. The command can be re-run by hand at any time.

## `status`

Reports the current association — SSID, BSSID, RSSI, channel, security, the
negotiated PHY mode (e.g. `802.11b/g/n`) and channel bandwidth (`20 MHz` or
`40 MHz`), and the configured TX power ceiling — plus the IP address, gateway
and netmask.

While an access point is running it reports that instead: the SSID, channel,
security and address being advertised, followed by each connected client with
its MAC address, DHCP-leased IP and signal strength.

## `off` and `on`

`off` powers the radio down: it stops the WiFi driver, and with it the PHY.
This is a real power-down rather than a disconnect — a station that has merely
disassociated still scans and still transmits, so a measurement taken against
one proves nothing.

The point is measurement, not power saving. On a board that shares a supply
between the radio and an analog front end, the radio is a suspect whenever a
reading is noisier than the part's datasheet says it should be, and the only
way to convict or clear it is to take it away and look again:

```
i2c nau7802 read 50         # with the radio up
wifi off
i2c nau7802 read 50         # with it genuinely silent
wifi on
```

The radio stays off until `on`. Commands that need it — `scan`, `connect`,
`ap`, `autostart`, `iperf` — refuse while it is off and name `wifi on` rather
than restarting it themselves, because silently powering the radio back up
would spoil the measurement the `off` was taken for. `status` reports the off
state instead of refusing.

**`off` takes the web console down with it**, since the address it is bound to
goes away. A serial session is the only way back in, which is worth knowing
before running it over the web console.

`on` powers the radio up and then does exactly what [`autostart`](#autostart)
does: rejoin the stored network, or raise the access point if there is none.

## `iperf <server>[:<port>]`

Runs an iperf2 TCP test against the specified server, defaulting to port 5001.
Before starting the transfer it prints a `Link:` line with the current RSSI,
PHY mode and bandwidth, so a slow run can be checked against the link it ran
over. Reports back throughput numbers. Optionally, the user may specify `continuous` which will cause the test to run continually, reporting results every 5 seconds.

A continuous run is ended with `wifi iperf stop`. Because commands are executed
one at a time, the test runs in the background and the prompt stays usable while
it reports.

## `netstats`

Reports lwIP's per-layer packet counters — received count, dropped count and
a summed error count (checksum, length, out-of-memory, routing, protocol and
option errors combined into one number) — for the link, IP, TCP and UDP
layers. Counts are cumulative since boot, not since the last command, so read
`netstats` before and after an `iperf` run and compare: a rising `drop` or
`err` count during the run points at packet loss or buffer pressure in the
network stack rather than the radio link (compare against `wifi status`'s
RSSI/PHY/bandwidth, which cover the radio side).
