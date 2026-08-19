#ifndef WIFI_H
#define WIFI_H

int cmd_wifi_scan(int argc, char **argv);
int cmd_wifi_connect(int argc, char **argv);
int cmd_wifi_ap(int argc, char **argv);
int cmd_wifi_status(int argc, char **argv);
int cmd_wifi_iperf(int argc, char **argv);
int cmd_wifi_netstats(int argc, char **argv);

/*
 * Power the radio down and back up. `off` stops the driver and the PHY, so a
 * measurement taken with it off is taken with the radio genuinely silent; it
 * stays off until `on`, which also rejoins the stored network.
 */
int cmd_wifi_off(int argc, char **argv);
int cmd_wifi_on(int argc, char **argv);

/*
 * Join the network stored in NVS, or host an access point when there is none.
 *
 * Queued by app_main() at boot so the web console is reachable without a serial
 * session. It runs as an ordinary command on the executor task, which is what
 * keeps it from racing anything the user types.
 */
int cmd_wifi_autostart(int argc, char **argv);

#endif
