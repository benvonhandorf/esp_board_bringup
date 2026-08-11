#include "console_io.h"

#include "esp_bringup.h"

#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

/*
 * stdin must be unbuffered and stdout line-buffered, otherwise linenoise sees
 * nothing until a full buffer's worth of input arrives and command output
 * appears in bursts.
 */
static void configure_stdio_buffering(void)
{
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

void console_io_init(void)
{
    /* Terminals emitting CR on Enter; we want CRLF going back out. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));

    /* Route stdin/stdout through the driver so reads block instead of spinning. */
    usb_serial_jtag_vfs_use_driver();

    configure_stdio_buffering();
}

const char *console_io_name(void)
{
    return "USB-Serial-JTAG";
}

bool console_io_host_connected(void)
{
    return usb_serial_jtag_is_connected();
}

#else /* UART console */

void console_io_init(void)
{
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

    uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(port, ESP_LINE_ENDINGS_CRLF);

    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /* The bootloader already configured the pins; only the driver is missing. */
    ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &uart_config));

    uart_vfs_dev_use_driver(port);

    configure_stdio_buffering();
}

const char *console_io_name(void)
{
    static char name[16];
    snprintf(name, sizeof(name), "UART%d", CONFIG_ESP_CONSOLE_UART_NUM);
    return name;
}

bool console_io_host_connected(void)
{
    /* A UART has no notion of an attached host. */
    return true;
}

#endif
