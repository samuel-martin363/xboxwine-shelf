#pragma once

#include <cstdint>
#include <string>

namespace xboxwine {

struct TransferSnapshot {
    bool running = false;
    std::uint64_t receivedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::string address;
    std::string status;
};

void StartTransferServer();
TransferSnapshot GetTransferSnapshot();

} // namespace xboxwine
