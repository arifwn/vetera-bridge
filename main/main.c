#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"
#include "btstack_stdio_esp32.h"
#include "nvs_flash.h"

extern int btstack_main(int argc, const char * argv[]);

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    btstack_init();

#if defined(CONFIG_ESP_CONSOLE_UART) || defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    btstack_stdio_init();
#endif

    btstack_main(0, NULL);
    btstack_run_loop_execute();
}
