#pragma once

#include <SKSE/SKSE.h>

namespace TFD::CompanionRestore
{
    void OnSkseMessage(SKSE::MessagingInterface::Message* m);
    std::size_t RestoreNow();
}
