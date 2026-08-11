#include "esp_bringup.h"
#include "output.h"
#include "repl.h"
#include "web.h"

#include "esp_http_server.h"
#include "mdns.h"

/* The page is compiled in via EMBED_FILES; no filesystem needed. */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

#define MDNS_HOSTNAME "esp-bringup"

/* Output chunks larger than this are split across frames. */
#define MAX_BROADCAST_CHUNK 512

/* Also the httpd socket limit: every open browser tab holds one WebSocket. */
#define MAX_OPEN_SOCKETS 7

static httpd_handle_t server;

/*
 * A chunk of console output on its way to the browsers.
 *
 * httpd_ws_send_frame_async() must run on the HTTP server's own task, so the
 * output sink (which runs on the command executor task) hands the text over
 * via httpd_queue_work() rather than sending directly.
 */
typedef struct {
    size_t len;
    char text[];
} broadcast_t;

static void broadcast_work(void *arg)
{
    broadcast_t *message = arg;

    size_t client_count = MAX_OPEN_SOCKETS;
    int client_fds[MAX_OPEN_SOCKETS];

    if (httpd_get_client_list(server, &client_count, client_fds) == ESP_OK) {
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)message->text,
            .len = message->len,
        };

        for (size_t i = 0; i < client_count; i++) {
            if (httpd_ws_get_fd_info(server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(server, client_fds[i], &frame);
            }
        }
    }

    free(message);
}

static void output_sink(const char *text, size_t len, void *ctx)
{
    (void)ctx;

    if (!server || len == 0) {
        return;
    }

    while (len > 0) {
        size_t chunk = len > MAX_BROADCAST_CHUNK ? MAX_BROADCAST_CHUNK : len;

        broadcast_t *message = malloc(sizeof(broadcast_t) + chunk);
        if (!message) {
            return; /* dropping output beats blocking the console */
        }

        message->len = chunk;
        memcpy(message->text, text, chunk);

        if (httpd_queue_work(server, broadcast_work, message) != ESP_OK) {
            free(message);
            return;
        }

        text += chunk;
        len -= chunk;
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Opening handshake; nothing to read yet. */
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT};

    /* Called with max_len 0 this only fills in the length. */
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0 || frame.len > 512) {
        return ESP_OK;
    }

    uint8_t *payload = calloc(1, frame.len + 1);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    frame.payload = payload;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) {
        /*
         * Hand off without echoing. Calling bp_printf() here would run the
         * output sink -- and therefore httpd_queue_work() -- on the HTTP
         * server's own task, which risks deadlocking against a full control
         * queue. The browser echoes its own input locally instead.
         */
        bp_console_submit((char *)payload, false);
    }

    free(payload);
    return err;
}

static const httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
};

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
};

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("ESP Board Bringup");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void bp_web_start(void)
{
    if (server) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    /* Every browser holds a WebSocket open, so the default of 4 sockets runs
     * out quickly with a couple of tabs. */
    config.max_open_sockets = MAX_OPEN_SOCKETS;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        server = NULL;
        bp_error("Starting the web server: %s", esp_err_to_name(err));
        return;
    }

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &ws_uri);

    bp_output_sink_register(output_sink, NULL);
    start_mdns();

    bp_printf("Web console at http://%s.local/ (or the IP address above)\n",
              MDNS_HOSTNAME);
}

void bp_web_stop(void)
{
    if (!server) {
        return;
    }

    bp_output_sink_unregister(output_sink, NULL);
    httpd_stop(server);
    server = NULL;
    mdns_free();
}
