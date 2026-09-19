#pragma once

#include "boards/common/ml307_board.h"
#include "../config.h"

#include <material_symbols.h>
#include <atomic>

// Board-local modem lifecycle, with cancellation checks and eDRX setup.
class BajiMl307 : public Ml307Board {
public:
    using Ml307Board::Ml307Board;
    void StartNetwork() override;
    void StopNetwork();
    NetworkInterface* GetNetwork() override;
    const char* GetNetworkStateIcon() override;
    std::string GetBoardJson() override;
    std::string GetDeviceStatusJson() override;

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    // Run owns and initializes modem_; readers only access it after publication.
    // The owning board retains this object until all interface users have stopped.
    std::atomic<AtModem*> published_modem_{nullptr};
    void Run();
    bool Delay(int milliseconds);
};

inline bool BajiMl307::Delay(int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds && !stopping_; elapsed += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !stopping_;
}

inline void BajiMl307::StartNetwork() {
    if (running_.exchange(true)) return;
    auto* modem = published_modem_.load(std::memory_order_acquire);
    if (modem && modem->network_ready()) {
        running_ = false;
        return;
    }
    stopping_ = false;
    if (xTaskCreate([](void* arg) {
            auto* self = static_cast<BajiMl307*>(arg);
            self->Run();
            self->running_ = false;
            vTaskDelete(nullptr);
        }, "baji_ml307", 8192, this, 5, nullptr) != pdPASS) {
        running_ = false;
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
    }
}

inline void BajiMl307::Run() {
    OnNetworkEvent(NetworkEvent::ModemDetecting);
    for (int attempt = 0; !modem_ && attempt < 30 && !stopping_; ++attempt) {
        modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 115200, 1200);
        if (!modem_ && !stopping_) {
            modem_ = AtModem::Detect(tx_pin_, rx_pin_, dtr_pin_, 921600, 1200);
        }
        if (!modem_) Delay(1000);
    }
    if (stopping_) return;
    if (!modem_) {
        OnNetworkEvent(NetworkEvent::ModemErrorInitFailed);
        return;
    }
    // StopNetwork enters flight mode. A PWRKEY pulse is not sufficient proof
    // that a still-responsive module has left that mode on the next start.
    modem_->SetFlightMode(false);
    if (stopping_) return;
    published_modem_.store(modem_.get(), std::memory_order_release);
#if ML307_ENABLE_EDRX
    for (int act : {ML307_EDRX_ACT, 9, 4, 5}) {
        if (stopping_) return;
        const auto command = std::string("AT+CEDRXS=2,") + std::to_string(act) + ",\"" ML307_EDRX_VALUE "\"";
        if (modem_->GetAtUart()->SendCommand(command, 2500)) break;
    }
#endif
    modem_->OnNetworkStateChanged([this](bool ready) {
        if (!stopping_) OnNetworkEvent(ready ? NetworkEvent::Connected : NetworkEvent::Disconnected);
    });
    OnNetworkEvent(NetworkEvent::Connecting);
    // Cancellation is checked between modem queries. The one-second argument
    // only bounds the final registration event wait; CPIN and AT queries inside
    // WaitForNetworkReady can take considerably longer when the modem is silent.
    for (int attempt = 0; attempt < 180 && !stopping_; ++attempt) {
        const auto status = modem_->WaitForNetworkReady(1000);
        if (stopping_) return;
        if (status == NetworkStatus::Ready) return;
        if (status == NetworkStatus::ErrorInsertPin) {
            OnNetworkEvent(NetworkEvent::ModemErrorNoSim);
            if (!Delay(10000)) return;
        } else if (status == NetworkStatus::ErrorRegistrationDenied) {
            OnNetworkEvent(NetworkEvent::ModemErrorRegDenied);
            if (!Delay(10000)) return;
        }
    }
    if (!stopping_) OnNetworkEvent(NetworkEvent::ModemErrorTimeout);
}

inline void BajiMl307::StopNetwork() {
    stopping_ = true;
    // Never delete a modem or its UART underneath the initialization task.
    while (running_) vTaskDelay(pdMS_TO_TICKS(20));
    if (modem_) modem_->SetFlightMode(true);
    // Keep the disconnected interface alive until the next switch. Protocol cleanup
    // is synchronized by the board before this call.
    OnNetworkEvent(NetworkEvent::Disconnected);
}

inline std::string BajiMl307::GetBoardJson() {
    if (published_modem_.load(std::memory_order_acquire) == nullptr) return "{}";
    return Ml307Board::GetBoardJson();
}

inline NetworkInterface* BajiMl307::GetNetwork() {
    return published_modem_.load(std::memory_order_acquire);
}

inline const char* BajiMl307::GetNetworkStateIcon() {
    if (published_modem_.load(std::memory_order_acquire) == nullptr) {
        return MATERIAL_SYMBOLS_ANDROID_CELL_4_BAR_OFF;
    }
    return Ml307Board::GetNetworkStateIcon();
}

inline std::string BajiMl307::GetDeviceStatusJson() {
    if (published_modem_.load(std::memory_order_acquire) == nullptr) return "{}";
    return Ml307Board::GetDeviceStatusJson();
}
