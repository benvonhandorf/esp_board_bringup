# All application sources are listed here. The subdirectories under main/ are
# plain source folders, NOT ESP-IDF components: IDF only auto-discovers
# components under components/ or EXTRA_COMPONENT_DIRS, so an
# idf_component_register() in main/gpio/ (etc.) would be silently ignored and
# those files would never be compiled.
idf_component_register(
    SRCS
        "main.c"
        "esp_bringup.c"
        "console/console_io.c"
        "console/output.c"
        "console/menu.c"
        "console/menu_table.c"
        "console/repl.c"
        "audio/audio.c"
        "audio/i2s_bus.c"
        "audio/tone.c"
        "audio/capture.c"
        "audio/codec_nau8822.c"
        "audio/codec_ns4168.c"
        "audio/codec_sph0645.c"
        "board/board.c"
        "gpio/gpio.c"
        "gpio/pwm.c"
        "i2c/i2c.c"
        "i2c/ina237.c"
        "i2c/sht4x.c"
        "i2c/nau7802.c"
        "sd/sd.c"
        "spi/spi.c"
        "system/system.c"
        "uart/uart.c"
        "wifi/wifi.c"
        "web/web.c"
    INCLUDE_DIRS
        "."
        "console"
        "audio"
        "board"
        "gpio"
        "i2c"
        "sd"
        "spi"
        "system"
        "uart"
        "wifi"
        "web"
    EMBED_FILES
        "web/index.html"
)

# Strict warnings for our code only; see the note in the top-level CMakeLists.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
