#pragma once

#include <esp_err.h>

namespace baji {

// Persist the request, then reboot before clearing any user data.
esp_err_t RequestFactoryReset();

// Run before application/network initialization, including charging-only boot.
esp_err_t CompletePendingFactoryReset();

}  // namespace baji
