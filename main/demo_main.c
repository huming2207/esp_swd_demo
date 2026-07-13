#include <inttypes.h>

#include <esp_log.h>
#include <swd_host.h>

void app_main(void)
{
    static const char *TAG = "main";
    ESP_LOGI(TAG, "Soul Injector Rev 6 SWD GPIO PoC");

    if (!swd_init_debug()) {
        ESP_LOGE(TAG, "SWD initialization failed");
        swd_off();
        return;
    }

    uint32_t idcode = 0;
    if (!swd_read_idcode(&idcode)) {
        ESP_LOGE(TAG, "DP IDCODE read failed");
        swd_off();
        return;
    }

    ESP_LOGI(TAG, "DP IDCODE: 0x%08" PRIx32, idcode);
    swd_off();
}
