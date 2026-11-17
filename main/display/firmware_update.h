#pragma once

enum class FirmwareUpdateStage {
    Confirm,
    Downloading,
    Verifying,
    Complete,
    Failed,
};
