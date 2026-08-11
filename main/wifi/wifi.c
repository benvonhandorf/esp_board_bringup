#include "esp_bringup.h"
#include "output.h"
#include "wifi.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "iperf.h"
#include "web.h"

#define MAX_SCAN_RESULTS 32

/* Long enough for a slow AP + DHCP, short enough not to wedge the console. */
#define CONNECT_TIMEOUT_MS 20000

/* README: continuous mode reports every 5 seconds until interrupted. */
#define IPERF_CONTINUOUS_INTERVAL_S 5
#define IPERF_SINGLE_DURATION_S 10
/* iperf's own time field is uint32; this stands in for "keep going". */
#define IPERF_CONTINUOUS_DURATION_S 86400

static bool wifi_started;
static esp_netif_t *sta_netif;
static EventGroupHandle_t wifi_events;
static iperf_id_t running_iperf = -1;

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
 * The README asks for connection state to be reported "including any
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

int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;

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

int cmd_wifi_connect(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: connect <AP> [password]\n");
        return -1;
    }

    esp_err_t err = ensure_wifi_started();
    if (err != ESP_OK) {
        bp_error("Starting WiFi: %s", esp_err_to_name(err));
        return -1;
    }

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, argv[1], sizeof(config.sta.ssid));
    if (argc > 2) {
        strlcpy((char *)config.sta.password, argv[2], sizeof(config.sta.password));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        bp_error("Applying WiFi config: %s", esp_err_to_name(err));
        return -1;
    }

    /* Drop any existing association first so `connect` can be re-run. */
    esp_wifi_disconnect();

    xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    connect_in_progress = true;

    bp_printf("Connecting to '%s'...\n", argv[1]);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        connect_in_progress = false;
        bp_error("Connect failed: %s", esp_err_to_name(err));
        return -1;
    }

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));

    connect_in_progress = false;

    if (bits & WIFI_CONNECTED_BIT) {
        /* The IP was already reported by the event handler. */
        return 0;
    }
    if (bits & WIFI_FAILED_BIT) {
        bp_error("Could not connect to '%s'", argv[1]);
        return -1;
    }

    bp_error("Timed out after %d seconds waiting for '%s'",
             CONNECT_TIMEOUT_MS / 1000, argv[1]);
    return -1;
}

int cmd_wifi_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!wifi_started) {
        bp_printf("WiFi is not started. Run 'wifi scan' or 'wifi connect' first.\n");
        return 0;
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

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK) {
        bp_printf("IP:       " IPSTR "\n", IP2STR(&ip.ip));
        bp_printf("Gateway:  " IPSTR "\n", IP2STR(&ip.gw));
        bp_printf("Netmask:  " IPSTR "\n", IP2STR(&ip.netmask));
    }

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
    uint32_t seconds = traffic->end_sec - traffic->period_start_sec;
    double mbits_per_sec = seconds ? (traffic->period_bytes * 8.0) /
                                     ((double)seconds * 1000000.0)
                                   : 0.0;

    bp_printf("%3lu-%3lu sec %12.2f KB %12.2f Mbit/s%s\n",
              (unsigned long)traffic->period_start_sec,
              (unsigned long)traffic->end_sec,
              traffic->period_bytes / 1024.0,
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
