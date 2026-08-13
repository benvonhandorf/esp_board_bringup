# WiFi

[← Command reference](README.md) · [Project README](../README.md)

This menu allows the user to perform actions on the wifi subsystem.

The radio is either a station or an access point, never both: a single radio
cannot sit on two channels, and an AP+station setup would silently drag the AP
onto whatever channel the station associated on. `connect` therefore stops a
running AP and `ap` drops a station association, each saying so as it happens.

## `scan`

Scans nearby wifi APs and outputs each AP including the RSSI and channel information for each.

Scanning is a station-mode operation, so it is refused while an access point is
running; stop it with `wifi ap stop` first.

## `connect <AP> [Password]`

Connects to the specified access point.  Connection status and IP address are reported to the user, including any disconnections or changes in the future.
Disconnect reasons are decoded to readable text. Credentials are stored in NVS,
so `autostart` rejoins the same network after a reset. Omit the password for an
open network. A running access point is stopped first.

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

Reports the current association — SSID, BSSID, RSSI, channel, security — plus
the IP address, gateway and netmask.

While an access point is running it reports that instead: the SSID, channel,
security and address being advertised, followed by each connected client with
its MAC address, DHCP-leased IP and signal strength.

## `iperf <server>[:<port>]`

Runs an iperf2 TCP test against the specified server, defaulting to port 5001.  Reports back throughput numbers.  Optionally, the user may specify `continuous` which will cause the test to run continually, reporting results every 5 seconds.

A continuous run is ended with `wifi iperf stop`. Because commands are executed
one at a time, the test runs in the background and the prompt stays usable while
it reports.
