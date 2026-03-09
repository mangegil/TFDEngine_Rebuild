#pragma once
#include <RE/Skyrim.h>

namespace TFD::CaptiveHostility
{
    // Call on UI thread. Aman dipanggil tiap tick; internalnya ada interval.
    void Tick();

    // Restore semua perubahan dan reset cache (panggil saat shutdown / reset state).
    void Reset();
}
