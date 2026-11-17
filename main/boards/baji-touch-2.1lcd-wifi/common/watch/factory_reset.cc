#include "factory_reset.h"

#include <nvs.h>
#include <nvs_flash.h>

namespace baji {
namespace {
constexpr char kNamespace[] = "factory_reset";
constexpr char kPendingKey[] = "pending";
}

esp_err_t RequestFactoryReset() {
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, kPendingKey, 1);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    return result;
}

esp_err_t CompletePendingFactoryReset() {
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result == ESP_OK) result = nvs_flash_init();
    }
    if (result != ESP_OK) return result;

    nvs_handle_t handle = 0;
    result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    uint8_t pending = 0;
    result = nvs_get_u8(handle, kPendingKey, &pending);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK || pending == 0) return result;
    if (pending != 1) return ESP_ERR_INVALID_STATE;

    // Erase at boot so active tasks and shutdown handlers cannot restore old data.
    // Only the default user NVS is cleared; firmware and built-in assets remain.
    result = nvs_flash_erase();
    if (result == ESP_OK) result = nvs_flash_init();
    return result;
}

}  // namespace baji
