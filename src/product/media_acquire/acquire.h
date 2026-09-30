#pragma once

#include "product/media_acquire/source.h"

#include <cstddef>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ninfer::product::media_acquire {

enum class ErrorKind {
    BudgetExceeded,
    RemoteUnavailable,
    RemoteTimeout,
    DeadlineExceeded,
    Cancelled,
};

class Error final : public std::runtime_error {
public:
    Error(ErrorKind kind, std::string message)
        : std::runtime_error(std::move(message)), kind_(kind) {}

    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }

private:
    ErrorKind kind_;
};

struct Policy {
    std::size_t max_bytes      = 256ULL << 20;
    int connect_timeout_ms     = 5'000;
    int timeout_ms             = 60'000;
    int max_redirects          = 3;
    bool allow_remote          = true;
    bool allow_private_network = false;
    std::filesystem::path media_root;
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> is_cancelled;
};

// A security-checked HTTP(S) response. DNS is resolved before each request and the selected
// address is pinned in libcurl, so every redirect is independently checked against Policy.
struct HttpResponse {
    std::vector<std::uint8_t> bytes;
    std::string content_type;
    std::string final_url;
    long status = 0;
};

HttpResponse acquire_http(std::string url, const Policy& policy = {});

std::vector<std::uint8_t> acquire_bytes(const Source& source, const Policy& policy = {});

} // namespace ninfer::product::media_acquire
