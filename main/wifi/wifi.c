#include "esp_bringup.h"
#include "output.h"
#include "wifi.h"

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/stats.h"

#include "iperf.h"
#include "web.h"

#define MAX_SCAN_RESULTS 32

/* Long enough for a slow AP + DHCP, short enough not to wedge the console. */
#define CONNECT_TIMEOUT_MS 20000

/* Shorter than the interactive timeout: at boot a board that cannot reach its
 * stored network should fall back to hosting its own AP promptly rather than
 * leaving the user staring at nothing for 20 seconds. */
#define BOOT_CONNECT_TIMEOUT_MS 10000

#define AP_DEFAULT_CHANNEL 1
#define AP_MAX_CLIENTS 4

/*
 * Field capacities taken from the driver's own config struct rather than
 * hard-coded. The SSID needs no terminator (ssid_len carries the length), the
 * password does.
 */
#define AP_SSID_MAX     (sizeof(((wifi_config_t *)0)->ap.ssid))
#define AP_PASSWORD_MAX (sizeof(((wifi_config_t *)0)->ap.password) - 1)

/* WPA2-PSK's own floor; the radio rejects anything shorter. */
#define AP_MIN_PASSWORD_LEN 8

/*
 * Credentials for the AP raised by `autostart` when no stored network can be
 * joined. Anyone with the source knows this password, so it is a speed bump
 * rather than security -- but an open console is worse on a shared bench.
 */
#define BOOT_AP_PASSWORD "bringup1234"

/* docs/wifi.md: continuous mode reports every 5 seconds until interrupted. */
#define IPERF_CONTINUOUS_INTERVAL_S 5
#define IPERF_SINGLE_DURATION_S 10
/* iperf's own time field is uint32; this stands in for "keep going". */
#define IPERF_CONTINUOUS_DURATION_S 86400

static bool wifi_started;
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static EventGroupHandle_t wifi_events;
static iperf_id_t running_iperf = -1;

/*
 * AP and station mode are deliberately exclusive: one radio cannot sit on two
 * channels, so an APSTA setup silently drags the AP onto whatever channel the
 * station associated on. Each mode drops the other and says so.
 */
static bool ap_active;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1

/* Set while a `connect` command is waiting, so asynchronous disconnect
 * reporting does not duplicate what the command itself prints. */
static bool connect_in_progress;

static const char *auth_mode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    case WIFI_AUTH_OWE:             return "OWE";
    default:                        return "unknown";
    }
}

/* Renders the negotiated PHY mode from a wifi_ap_record_t's bitfields, so a
 * link stuck on an older/narrower mode than the AP supports is visible at a
 * glance -- useful when throughput is lower than expected and RF is one of
 * the suspects. */
static const char *phy_mode_name(const wifi_ap_record_t *ap)
{
    static char name[32];
    char *p = name;
    memcpy(p, "802.11", 6);
    p += 6;
    bool first = true;

    const struct { bool set; const char *tag; } modes[] = {
        {ap->phy_11b,  "b"},
        {ap->phy_11g,  "g"},
        {ap->phy_11n,  "n"},
        {ap->phy_11ax, "ax"},
    };

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!modes[i].set) {
            continue;
        }
        if (!first) {
            *p++ = '/';
        }
        size_t len = strlen(modes[i].tag);
        memcpy(p, modes[i].tag, len);
        p += len;
        first = false;
    }

    if (first) {
        return "unknown";
    }

    if (ap->phy_lr) {
        memcpy(p, "+LR", 3);
        p += 3;
    }

    *p = '\0';
    return name;
}

static const char *bandwidth_name(wifi_bandwidth_t bw)
{
    switch (bw) {
    case WIFI_BW20: return "20 MHz";
    case WIFI_BW40: return "40 MHz";
    default:        return "unknown";
    }
}

static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:        return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE:         return "left the network";
    case WIFI_REASON_ASSOC_LEAVE:        return "deassociated on leaving";
    case WIFI_REASON_ASSOC_NOT_AUTHED:   return "associated but not authenticated";
    case WIFI_REASON_ASSOC_TOOMANY:      return "access point has too many clients";
    case WIFI_REASON_BEACON_TIMEOUT:     return "beacon timeout (out of range?)";
    case WIFI_REASON_NO_AP_FOUND:        return "no such access point";
    case WIFI_REASON_AUTH_FAIL:          return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:         return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "handshake timeout (wrong password?)";
    case WIFI_REASON_CONNECTION_FAIL:    return "connection failed";
    default:                             return "see esp_wifi_types.h";
    }
}

/*
 * docs/wifi.md asks for connection state to be reported "including any
 * disconnections or changes in the future", so these run for the lifetime of
 * the program rather than only during `connect`.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;

        bp_printf("WiFi: disconnected from '%.*s' (reason %d: %s)\n",
                  event->ssid_len, (const char *)event->ssid,
                  event->reason, disconnect_reason_name(event->reason));

        if (connect_in_progress) {
            xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;

        bp_printf("WiFi: got IP " IPSTR " (gateway " IPSTR ", mask " IPSTR ")\n",
                  IP2STR(&event->ip_info.ip),
                  IP2STR(&event->ip_info.gw),
                  IP2STR(&event->ip_info.netmask));

        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);

        /* The web console can only listen once there is an address to bind. */
        bp_web_start();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        bp_printf("WiFi: lost IP address\n");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        /* Unlike a station, the AP owns its address the moment it is up, so
         * there is no DHCP lease to wait for before serving. */
        bp_web_start();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = event_data;

        bp_printf("WiFi: client " MACSTR " joined the access point (AID %u)\n",
                  MAC2STR(event->mac), event->aid);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = event_data;

        bp_printf("WiFi: client " MACSTR " left the access point "
                  "(AID %u, reason %u: %s)\n",
                  MAC2STR(event->mac), event->aid, event->reason,
                  disconnect_reason_name((uint8_t)event->reason));
    }
}

/* Bring the stack up once, on first use, so a board that is never networked
 * does not pay for the radio. */
static esp_err_t ensure_wifi_started(void)
{
    if (wifi_started) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    wifi_events = xEventGroupCreate();
    if (!wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    /* Credentials live in NVS so a reconnect survives a reset. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_started = true;
    return ESP_OK;
}

/* Created on first use, so a board that never hosts an AP pays for neither the
 * netif nor the DHCP server behind it. */
static esp_err_t ensure_ap_netif(void)
{
    if (ap_netif) {
        return ESP_OK;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    return ap_netif ? ESP_OK : ESP_ERR_NO_MEM;
}

/*
 * Bring the soft AP up. Shared by `ap` and `autostart`.
 *
 * An empty password means an open network. The caller has already validated
 * lengths; what is left here is the mode switch, which drops any station
 * association because the two modes are exclusive.
 */
static int start_ap(const char *ssid, const char *password, int channel)
{
    esp_err_t err = ensure_wifi_started();
    if (err != ESP_OK) {
        bp_error("Starting WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    err = ensure_ap_netif();
    if (err != ESP_OK) {
        bp_error("Creating the AP interface: %s", esp_err_to_name(err));
        return -1;
    }

    wifi_ap_record_t associated;
    if (esp_wifi_sta_get_ap_info(&associated) == ESP_OK) {
        bp_printf("Leaving '%s'; access point mode is exclusive\n",
                  (const char *)associated.ssid);
    }

    /* Unconditional: this also cancels an attempt still in flight, such as the
     * failed autostart connect that led here. */
    esp_wifi_disconnect();

    wifi_config_t config = {0};
    size_t ssid_len = strlen(ssid);

    memcpy(config.ap.ssid, ssid, ssid_len);
    config.ap.ssid_len = (uint8_t)ssid_len;
    config.ap.channel = (uint8_t)channel;
    config.ap.max_connection = AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_OPEN;

    if (password[0]) {
        strlcpy((char *)config.ap.password, password, sizeof(config.ap.password));
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        /* Capable but not required, so older clients still associate. */
        config.ap.pmf_cfg.capable = true;
        config.ap.pmf_cfg.required = false;
    }

    /*
     * Stop, reconfigure, start -- the order the driver expects.
     *
     * esp_wifi_set_config() rejects an interface that is not active, so the mode
     * has to change first; but changing mode on a *running* stack starts the AP
     * immediately, which would briefly beacon whatever SSID was left in NVS (or
     * a blank one on a fresh board) before our config lands. Stopping first
     * means the AP only ever advertises what was asked for.
     */
    esp_wifi_stop();

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        bp_error("Starting the access point: %s", esp_err_to_name(err));
        /* Leave the radio somewhere predictable rather than half-configured. */
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        return -1;
    }

    ap_active = true;

    bp_printf("Access point '%s' up on channel %d (%s)\n", ssid, channel,
              password[0] ? "WPA2" : "open");
    if (password[0]) {
        bp_printf("Password: %s\n", password);
    }

    /* The web server is started by the AP_START event; report where to find it. */
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        bp_printf("AP IP:    " IPSTR "\n", IP2STR(&ip.ip));
    }

    return 0;
}

/*
 * Join a configured network and wait for the outcome. The caller has already
 * applied the credentials with esp_wifi_set_config().
 */
static int connect_and_wait(const char *ssid, int timeout_ms)
{
    /* Drop any existing association first so a connect can be re-run. */
    esp_wifi_disconnect();

    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    connect_in_progress = true;

    bp_printf("Connecting to '%s'...\n", ssid);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        connect_in_progress = false;
        bp_error("Connect failed: %s", esp_err_to_name(err));
        return -1;
    }

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    connect_in_progress = false;

    if (bits & WIFI_CONNECTED_BIT) {
        /* The IP was already reported by the event handler. */
        return 0;
    }
    if (bits & WIFI_FAILED_BIT) {
        bp_error("Could not connect to '%s'", ssid);
        return -1;
    }

    bp_error("Timed out after %d seconds waiting for '%s'", timeout_ms / 1000, ssid);
    return -1;
}

/* Leave AP mode so a station operation can proceed. */
static esp_err_t leave_ap_mode(void)
{
    if (!ap_active) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    ap_active = false;
    return ESP_OK;
}

int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* The radio can only scan as a station, and AP mode is exclusive. Say so
     * rather than surfacing a bare ESP_ERR_WIFI_MODE. */
    if (ap_active) {
        bp_error("Scanning requires station mode. Run 'wifi ap stop' first");
        return -1;
    }

    esp_err_t err = ensure_wifi_started();
    if (err != ESP_OK) {
        bp_error("Starting WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    bp_printf("Scanning...\n");

    err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        bp_error("Scan failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t count = MAX_SCAN_RESULTS;
    wifi_ap_record_t *records = calloc(count, sizeof(wifi_ap_record_t));
    if (!records) {
        bp_error("Out of memory");
        return -1;
    }

    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        bp_error("Reading scan results: %s", esp_err_to_name(err));
        free(records);
        return -1;
    }

    if (count == 0) {
        bp_printf("No access points found\n");
        free(records);
        return 0;
    }

    bp_printf("%-32s %5s %4s  %-10s %s\n", "SSID", "RSSI", "CH", "SECURITY", "BSSID");
    for (uint16_t i = 0; i < count; i++) {
        const wifi_ap_record_t *ap = &records[i];
        bp_printf("%-32s %4d %4d  %-10s %02x:%02x:%02x:%02x:%02x:%02x\n",
                  (const char *)ap->ssid, ap->rssi, ap->primary,
                  auth_mode_name(ap->authmode),
                  ap->bssid[0], ap->bssid[1], ap->bssid[2],
                  ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    }
    bp_printf("%u access point%s found\n", count, count == 1 ? "" : "s");

    free(records);
    return 0;
}

/* "aa:bb:cc:dd:ee:ff" -> 6 raw bytes. Returns 0 on success, -1 on any
 * malformed input (wrong octet count, non-hex, trailing garbage). */
static int parse_bssid(const char *token, uint8_t bssid[6])
{
    unsigned b[6];
    int consumed = 0;

    if (sscanf(token, "%2x:%2x:%2x:%2x:%2x:%2x%n",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &consumed) != 6 ||
        token[consumed] != '\0') {
        return -1;
    }

    for (int i = 0; i < 6; i++) {
        bssid[i] = (uint8_t)b[i];
    }
    return 0;
}

int cmd_wifi_connect(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: connect <AP> [password] [channel] [bssid]\n");
        return -1;
    }

    esp_err_t err = ensure_wifi_started();
    if (err != ESP_OK) {
        bp_error("Starting WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    if (ap_active) {
        bp_printf("Stopping the access point; station mode is exclusive\n");
        err = leave_ap_mode();
        if (err != ESP_OK) {
            bp_error("Switching to station mode: %s", esp_err_to_name(err));
            return -1;
        }
    }

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, argv[1], sizeof(config.sta.ssid));
    if (argc > 2) {
        strlcpy((char *)config.sta.password, argv[2], sizeof(config.sta.password));
    }

    /* An empty channel ("") is how you skip the channel hint but still
     * reach the bssid argument, matching how `ap` treats an empty password. */
    if (argc > 3 && argv[3][0] != '\0') {
        int channel;
        if (parse_int_arg(argv[3], &channel) < 0 || channel < 1 || channel > 13) {
            bp_error("Channel must be 1-13");
            return -1;
        }
        config.sta.channel = (uint8_t)channel;
    }

    if (argc > 4) {
        if (parse_bssid(argv[4], config.sta.bssid) < 0) {
            bp_error("BSSID must be six colon-separated hex octets, e.g. aa:bb:cc:dd:ee:ff");
            return -1;
        }
        config.sta.bssid_set = true;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        bp_error("Applying WiFi config: %s", esp_err_to_name(err));
        return -1;
    }

    return connect_and_wait(argv[1], CONNECT_TIMEOUT_MS);
}

int cmd_wifi_ap(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: ap <SSID> [password] [channel]\n");
        bp_printf("       ap stop\n");
        return -1;
    }

    if (strcasecmp(argv[1], "stop") == 0) {
        if (!ap_active) {
            bp_error("No access point is running");
            return -1;
        }

        esp_err_t err = leave_ap_mode();
        if (err != ESP_OK) {
            bp_error("Stopping the access point: %s", esp_err_to_name(err));
            return -1;
        }

        /* The web server stays bound; it becomes reachable again as soon as a
         * station association provides an address. */
        bp_printf("Access point stopped\n");
        return 0;
    }

    const char *ssid = argv[1];
    size_t ssid_len = strlen(ssid);

    if (ssid_len == 0 || ssid_len > AP_SSID_MAX) {
        bp_error("SSID must be 1-%u characters", (unsigned)AP_SSID_MAX);
        return -1;
    }

    const char *password = argc > 2 ? argv[2] : "";
    size_t password_len = strlen(password);

    /* An empty password is how you ask for an open network -- which also lets
     * `ap <ssid> "" 6` reach the channel argument. */
    if (password_len > 0 &&
        (password_len < AP_MIN_PASSWORD_LEN || password_len > AP_PASSWORD_MAX)) {
        bp_error("Password must be %d-%u characters, or empty for an open network",
                 AP_MIN_PASSWORD_LEN, (unsigned)AP_PASSWORD_MAX);
        return -1;
    }

    int channel = AP_DEFAULT_CHANNEL;
    if (argc > 3) {
        if (parse_int_arg(argv[3], &channel) < 0 || channel < 1 || channel > 13) {
            bp_error("Channel must be 1-13");
            return -1;
        }
    }

    return start_ap(ssid, password, channel);
}

/* "esp-bringup-a4cf12", from the low three bytes of the soft AP MAC. */
static void default_ap_ssid(char *out, size_t len)
{
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "esp-bringup-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

int cmd_wifi_autostart(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    esp_err_t err = ensure_wifi_started();
    if (err != ESP_OK) {
        bp_error("Starting WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    /*
     * Credentials are kept in NVS (esp_wifi_set_storage(WIFI_STORAGE_FLASH)),
     * so esp_wifi_init() has already reloaded the last successful `connect`. A
     * non-empty SSID means there is a network worth trying.
     */
    wifi_config_t stored = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK && stored.sta.ssid[0]) {
        if (connect_and_wait((const char *)stored.sta.ssid,
                             BOOT_CONNECT_TIMEOUT_MS) == 0) {
            return 0;
        }
        bp_printf("WiFi: falling back to the access point\n");
    } else {
        bp_printf("WiFi: no stored network, starting the access point\n");
    }

    char ssid[AP_SSID_MAX + 1];
    default_ap_ssid(ssid, sizeof(ssid));

    return start_ap(ssid, BOOT_AP_PASSWORD, AP_DEFAULT_CHANNEL);
}

/* The AP half of `status`: what we are advertising, and who has joined. */
static int report_ap_status(void)
{
    wifi_config_t config = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        bp_error("Reading the AP config: %s", esp_err_to_name(err));
        return -1;
    }

    bp_printf("Mode:     access point\n");
    bp_printf("SSID:     %.*s\n", config.ap.ssid_len, (const char *)config.ap.ssid);
    bp_printf("Channel:  %u\n", config.ap.channel);
    bp_printf("Security: %s\n", auth_mode_name(config.ap.authmode));

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        bp_printf("IP:       " IPSTR "\n", IP2STR(&ip.ip));
        bp_printf("Netmask:  " IPSTR "\n", IP2STR(&ip.netmask));
    }

    wifi_sta_list_t stations;
    if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) {
        return 0;
    }

    bp_printf("Clients:  %d of %d\n", stations.num, AP_MAX_CLIENTS);
    if (stations.num == 0) {
        return 0;
    }

    /* Pair the associations with the DHCP leases, so a client can be found by
     * address and not just by MAC. */
    wifi_sta_mac_ip_list_t leases = {0};
    bool have_ips = esp_wifi_ap_get_sta_list_with_ip(&stations, &leases) == ESP_OK;

    for (int i = 0; i < stations.num; i++) {
        if (have_ips) {
            bp_printf("  " MACSTR "  " IPSTR "  %d dBm\n",
                      MAC2STR(stations.sta[i].mac), IP2STR(&leases.sta[i].ip),
                      stations.sta[i].rssi);
        } else {
            bp_printf("  " MACSTR "  %d dBm\n", MAC2STR(stations.sta[i].mac),
                      stations.sta[i].rssi);
        }
    }

    return 0;
}

int cmd_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!wifi_started) {
        bp_printf("WiFi is not started. Run 'wifi scan' or 'wifi connect' first.\n");
        return 0;
    }

    if (ap_active) {
        return report_ap_status();
    }

    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err != ESP_OK) {
        bp_printf("Not associated with an access point\n");
        return 0;
    }

    bp_printf("SSID:     %s\n", (const char *)ap.ssid);
    bp_printf("BSSID:    %02x:%02x:%02x:%02x:%02x:%02x\n",
              ap.bssid[0], ap.bssid[1], ap.bssid[2],
              ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    bp_printf("RSSI:     %d dBm\n", ap.rssi);
    bp_printf("Channel:  %d\n", ap.primary);
    bp_printf("Security: %s\n", auth_mode_name(ap.authmode));
    bp_printf("PHY:      %s\n", phy_mode_name(&ap));
    bp_printf("Bandwidth: %s\n", bandwidth_name(ap.bandwidth));

    int8_t tx_power = 0;
    if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK) {
        bp_printf("TX power: %d dBm (configured ceiling)\n", tx_power / 4);
    }

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
        bp_printf("IP:       " IPSTR "\n", IP2STR(&ip.ip));
        bp_printf("Gateway:  " IPSTR "\n", IP2STR(&ip.gw));
        bp_printf("Netmask:  " IPSTR "\n", IP2STR(&ip.netmask));
    }

    return 0;
}

/* lwIP splits each layer's errors into six causes; summed into one count to
 * keep the netstats output as terse as the rest of the wifi menu. */
static uint32_t proto_errors(const struct stats_proto *s)
{
    return s->chkerr + s->lenerr + s->memerr + s->rterr + s->proterr + s->err;
}

int cmd_wifi_netstats(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bp_printf("Link:     rx %-8u drop %-6u err %lu\n",
              lwip_stats.link.recv, lwip_stats.link.drop, (unsigned long)proto_errors(&lwip_stats.link));
    bp_printf("IP:       rx %-8u drop %-6u err %lu\n",
              lwip_stats.ip.recv, lwip_stats.ip.drop, (unsigned long)proto_errors(&lwip_stats.ip));
    bp_printf("TCP:      rx %-8u drop %-6u err %lu\n",
              lwip_stats.tcp.recv, lwip_stats.tcp.drop, (unsigned long)proto_errors(&lwip_stats.tcp));
    bp_printf("UDP:      rx %-8u drop %-6u err %lu\n",
              lwip_stats.udp.recv, lwip_stats.udp.drop, (unsigned long)proto_errors(&lwip_stats.udp));

    return 0;
}

/*
 * Override the iperf component's weak report hook so throughput lines go
 * through the mirroring layer and therefore reach the web interface too. The
 * default implementation prints straight to stdout.
 */
void iperf_report_output(const iperf_report_t *report)
{
    if (report->report_type == IPERF_REPORT_CONNECT_INFO) {
        bp_printf("iperf: connected (socket %d)\n", report->connect_info.socket);
        bp_printf("%8s %16s %16s\n", "Interval", "Transfer", "Bandwidth");
        return;
    }

    const iperf_traffic_report_t *traffic = &report->traffic;
    unsigned long start_sec = (report->report_type == IPERF_REPORT_SUMMARY) ? 0 : traffic->period_start_sec;
    uint32_t seconds = traffic->end_sec - start_sec;
    double bytes = (report->report_type == IPERF_REPORT_SUMMARY) ? traffic->total_transfer_bytes : traffic->period_bytes;
    double mbits_per_sec = seconds ? (bytes * 8.0) / ((double)seconds * 1000000.0) : 0.0;

    bp_printf("%3lu-%3lu sec %12.2f KB %12.2f Mbit/s%s\n",
              start_sec,
              (unsigned long)traffic->end_sec,
              bytes / 1024.0,
              mbits_per_sec,
              report->report_type == IPERF_REPORT_SUMMARY ? "  (total)" : "");
}

/* Parse "<host>:<port>", where the port is optional. */
static int parse_target(const char *text, esp_ip4_addr_t *addr, uint16_t *port)
{
    char host[64];
    const char *colon = strrchr(text, ':');

    if (colon) {
        size_t host_len = (size_t)(colon - text);
        if (host_len == 0 || host_len >= sizeof(host)) {
            return -1;
        }
        memcpy(host, text, host_len);
        host[host_len] = '\0';

        int value;
        if (parse_int_arg(colon + 1, &value) < 0 || value < 1 || value > 65535) {
            return -1;
        }
        *port = (uint16_t)value;
    } else {
        if (strlen(text) >= sizeof(host)) {
            return -1;
        }
        strcpy(host, text);
        *port = IPERF_DEFAULT_PORT;
    }

    /* Accept a dotted quad directly, otherwise resolve the name. */
    struct in_addr parsed;
    if (inet_aton(host, &parsed)) {
        addr->addr = parsed.s_addr;
        return 0;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(host, NULL, &hints, &result) != 0 || !result) {
        return -1;
    }

    addr->addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);
    return 0;
}

int cmd_wifi_iperf(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: iperf <server>[:<port>] [continuous]\n");
        bp_printf("       iperf stop\n");
        return -1;
    }

    if (strcasecmp(argv[1], "stop") == 0) {
        if (running_iperf < 0) {
            bp_error("No iperf test is running");
            return -1;
        }
        iperf_stop_instance(running_iperf);
        running_iperf = -1;
        bp_printf("iperf stopped\n");
        return 0;
    }

    if (!wifi_started) {
        bp_error("WiFi is not connected. Run 'wifi connect <AP> <password>' first.");
        return -1;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        bp_error("Not associated with an access point");
        return -1;
    }

    bp_printf("Link:     RSSI %d dBm, %s, %s\n",
              ap.rssi, phy_mode_name(&ap), bandwidth_name(ap.bandwidth));

    bool continuous = (argc > 2 && strcasecmp(argv[2], "continuous") == 0);

    esp_ip4_addr_t server = {0};
    uint16_t port = IPERF_DEFAULT_PORT;
    if (parse_target(argv[1], &server, &port) < 0) {
        bp_error("Could not resolve '%s'. Expected <server>[:<port>]", argv[1]);
        return -1;
    }

    if (running_iperf >= 0) {
        iperf_stop_instance(running_iperf);
        running_iperf = -1;
    }

    esp_ip_addr_t destination = {0};
    destination.u_addr.ip4 = server;
    destination.type = ESP_IPADDR_TYPE_V4;

    iperf_cfg_t config = IPERF_DEFAULT_CONFIG_CLIENT(IPERF_FLAG_TCP, destination);
    config.dport = port;
    config.interval = continuous ? IPERF_CONTINUOUS_INTERVAL_S : IPERF_DEFAULT_INTERVAL;
    config.time = continuous ? IPERF_CONTINUOUS_DURATION_S : IPERF_SINGLE_DURATION_S;

    bp_printf("iperf2 TCP client -> " IPSTR ":%u, reporting every %lu s\n",
              IP2STR(&server), port, (unsigned long)config.interval);

    running_iperf = iperf_start_instance(&config);
    if (running_iperf < 0) {
        bp_error("Could not start iperf");
        return -1;
    }

    if (continuous) {
        bp_printf("Running continuously. Enter 'wifi iperf stop' to end the test.\n");
        return 0;
    }

    /* iperf runs in its own task; wait for the fixed-duration run to finish so
     * the prompt does not come back before the summary. */
    vTaskDelay(pdMS_TO_TICKS((IPERF_SINGLE_DURATION_S + 2) * 1000));
    iperf_stop_instance(running_iperf);
    running_iperf = -1;
    return 0;
}
