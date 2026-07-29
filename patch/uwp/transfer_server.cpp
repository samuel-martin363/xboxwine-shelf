/*
 * Local-network game transfer receiver for XboxWine Shelf.
 * GPL-2.0-or-later
 */

#include "transfer_server.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace xboxwine {
namespace {

using namespace winrt;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

constexpr wchar_t kPort[] = L"24872";
constexpr std::array<std::uint8_t, 8> kMagic{
    'X', 'W', 'U', 'P', '2', 0, 0, 0
};
constexpr std::uint32_t kMaximumManifestBytes = 1024 * 1024;

std::once_flag gStartOnce;
std::atomic<bool> gRunning{false};
std::mutex gStatusMutex;
TransferSnapshot gSnapshot;

void SetStatus(
    const std::string& status,
    std::uint64_t received = 0,
    std::uint64_t total = 0
) {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    gSnapshot.running = gRunning.load();
    gSnapshot.status = status;
    gSnapshot.receivedBytes = received;
    gSnapshot.totalBytes = total;
}

std::string LocalIPv4() {
    try {
        for (const HostName& host : NetworkInformation::GetHostNames()) {
            if (host.Type() != HostNameType::Ipv4) {
                continue;
            }
            const auto information = host.IPInformation();
            if (!information || !information.NetworkAdapter()) {
                continue;
            }
            return to_string(host.CanonicalName());
        }
    } catch (...) {
    }
    return "XBOX-IP";
}

std::string Trim(const std::string& value) {
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }
    );
    if (first == value.end()) {
        return {};
    }
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }
    ).base();
    return std::string(first, last);
}

std::string ManifestValue(const std::string& text, const std::string& key) {
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = Trim(text.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start
        ));
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) {
            std::string found = Trim(line.substr(0, equals));
            std::transform(
                found.begin(), found.end(), found.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                }
            );
            if (found == key) {
                return Trim(line.substr(equals + 1));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

std::string SafeFolderName(const std::string& title) {
    std::string result;
    result.reserve(title.size());
    for (unsigned char c : title) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(c));
        }
    }
    result = Trim(result);
    if (result.empty()) {
        result = "Imported Game";
    }
    if (result.size() > 80) {
        result.resize(80);
    }
    return result;
}

std::vector<std::uint8_t> ReadExact(
    DataReader& reader,
    std::uint32_t byteCount
) {
    const std::uint32_t loaded = reader.LoadAsync(byteCount).get();
    if (loaded != byteCount) {
        throw std::runtime_error("The PC disconnected during transfer.");
    }
    std::vector<std::uint8_t> bytes(byteCount);
    reader.ReadBytes(bytes);
    return bytes;
}

void SendReply(const StreamSocket& socket, const std::string& text) {
    try {
        DataWriter writer(socket.OutputStream());
        writer.UnicodeEncoding(UnicodeEncoding::Utf8);
        writer.WriteString(to_hstring(text));
        writer.StoreAsync().get();
        writer.FlushAsync().get();
        writer.DetachStream();
    } catch (...) {
    }
}

void ReceiveGame(const StreamSocket& socket) {
    try {
        SetStatus("PC CONNECTED - READING GAME HEADER");

        DataReader reader(socket.InputStream());
        reader.ByteOrder(ByteOrder::LittleEndian);
        reader.InputStreamOptions(InputStreamOptions::None);

        const auto fixedHeader = ReadExact(reader, 20);
        if (!std::equal(kMagic.begin(), kMagic.end(), fixedHeader.begin())) {
            throw std::runtime_error("Invalid XboxWine upload protocol.");
        }

        auto readU32 = [&](std::size_t offset) -> std::uint32_t {
            return static_cast<std::uint32_t>(fixedHeader[offset]) |
                   (static_cast<std::uint32_t>(fixedHeader[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(fixedHeader[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(fixedHeader[offset + 3]) << 24);
        };
        auto readU64 = [&](std::size_t offset) -> std::uint64_t {
            std::uint64_t result = 0;
            for (int index = 0; index < 8; ++index) {
                result |= static_cast<std::uint64_t>(fixedHeader[offset + index])
                          << (index * 8);
            }
            return result;
        };

        const std::uint32_t manifestLength = readU32(8);
        const std::uint64_t archiveLength = readU64(12);
        if (manifestLength == 0 || manifestLength > kMaximumManifestBytes) {
            throw std::runtime_error("The upload manifest is invalid.");
        }
        if (archiveLength == 0) {
            throw std::runtime_error("The selected folder produced an empty archive.");
        }

        const auto manifestBytes = ReadExact(reader, manifestLength);
        const std::string manifest(
            manifestBytes.begin(), manifestBytes.end()
        );
        const std::string title = ManifestValue(manifest, "title");
        if (title.empty()) {
            throw std::runtime_error("The upload has no game title.");
        }

        const StorageFolder local = ApplicationData::Current().LocalFolder();
        const StorageFolder games = local.CreateFolderAsync(
            L"Games",
            CreationCollisionOption::OpenIfExists
        ).get();
        const StorageFolder game = games.CreateFolderAsync(
            to_hstring(SafeFolderName(title)),
            CreationCollisionOption::OpenIfExists
        ).get();
        const StorageFile archive = game.CreateFileAsync(
            L"game.zip",
            CreationCollisionOption::ReplaceExisting
        ).get();

        const IRandomAccessStream output = archive.OpenAsync(
            FileAccessMode::ReadWrite
        ).get();
        output.Size(0);
        DataWriter writer(output.GetOutputStreamAt(0));

        std::uint64_t remaining = archiveLength;
        std::uint64_t received = 0;
        constexpr std::uint32_t chunkSize = 1024 * 1024;
        while (remaining > 0) {
            const std::uint32_t requested = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(remaining, chunkSize)
            );
            const std::uint32_t loaded = reader.LoadAsync(requested).get();
            if (loaded == 0) {
                throw std::runtime_error("The PC disconnected during game transfer.");
            }
            std::vector<std::uint8_t> chunk(loaded);
            reader.ReadBytes(chunk);
            writer.WriteBytes(chunk);
            writer.StoreAsync().get();
            received += loaded;
            remaining -= loaded;
            SetStatus("RECEIVING " + title, received, archiveLength);
        }
        writer.FlushAsync().get();
        writer.DetachStream();

        const StorageFile manifestFile = game.CreateFileAsync(
            L"game.xwgame",
            CreationCollisionOption::ReplaceExisting
        ).get();
        FileIO::WriteTextAsync(
            manifestFile,
            to_hstring(manifest),
            UnicodeEncoding::Utf8
        ).get();

        SetStatus("RECEIVED " + title + " - PRESS B TO RETURN", archiveLength, archiveLength);
        SendReply(socket, "OK\n");
    } catch (const hresult_error& error) {
        const std::string message = "TRANSFER FAILED: " + to_string(error.message());
        SetStatus(message);
        SendReply(socket, "ERROR " + message + "\n");
    } catch (const std::exception& error) {
        const std::string message = "TRANSFER FAILED: " + std::string(error.what());
        SetStatus(message);
        SendReply(socket, "ERROR " + message + "\n");
    }
}

void ServerThread() {
    try {
        init_apartment(apartment_type::multi_threaded);
        StreamSocketListener listener;
        listener.Control().KeepAlive(true);
        listener.ConnectionReceived(
            [](const StreamSocketListener&, const StreamSocketListenerConnectionReceivedEventArgs& args) {
                ReceiveGame(args.Socket());
            }
        );
        listener.BindServiceNameAsync(kPort).get();

        gRunning.store(true);
        {
            std::lock_guard<std::mutex> lock(gStatusMutex);
            gSnapshot.running = true;
            gSnapshot.address = LocalIPv4() + ":24872";
            gSnapshot.status = "READY FOR A GAME FOLDER FROM YOUR PC";
            gSnapshot.receivedBytes = 0;
            gSnapshot.totalBytes = 0;
        }

        while (gRunning.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const hresult_error& error) {
        gRunning.store(false);
        SetStatus("TRANSFER SERVER FAILED: " + to_string(error.message()));
    } catch (const std::exception& error) {
        gRunning.store(false);
        SetStatus("TRANSFER SERVER FAILED: " + std::string(error.what()));
    }
}

} // namespace

void StartTransferServer() {
    std::call_once(gStartOnce, []() {
        std::thread(ServerThread).detach();
    });
}

TransferSnapshot GetTransferSnapshot() {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    TransferSnapshot result = gSnapshot;
    result.running = gRunning.load();
    return result;
}

} // namespace xboxwine
