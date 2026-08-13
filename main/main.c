#include "esp_bringup.h"
#include "console/repl.h"

void app_main(void)
{
    bp_console_start();

    /*
     * Bring the network up as a queued command rather than inline: the executor
     * task is already running, so this keeps the boot sequence on the same
     * single-threaded path as everything the user types instead of racing it.
     * wait=false so the prompt appears while the connect attempt is in flight.
     */
    bp_console_submit("wifi autostart", false);
}
