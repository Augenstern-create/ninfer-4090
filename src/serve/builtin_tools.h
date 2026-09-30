#pragma once

#include "serve/request.h"
#include "serve/serve_options.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ninfer::serve {

struct GenerationOutcome;

inline constexpr std::string_view kWebSearchToolName = "web_search";
inline constexpr std::string_view kWebOpenToolName   = "web_open";

struct BuiltinToolControl {
    std::function<bool()> is_cancelled;
};

class BuiltinTool {
public:
    virtual ~BuiltinTool()                                                             = default;
    [[nodiscard]] virtual const ToolDefinition& definition() const noexcept            = 0;
    [[nodiscard]] virtual std::string execute(std::string_view arguments_json,
                                              const BuiltinToolControl& control) const = 0;
};

struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

class WebSearchBackend {
public:
    virtual ~WebSearchBackend() = default;
    [[nodiscard]] virtual std::vector<WebSearchResult>
    search(std::string_view query, std::size_t max_results, std::chrono::milliseconds timeout,
           const BuiltinToolControl& control) const = 0;
};

class SearXNGWebSearchBackend final : public WebSearchBackend {
public:
    explicit SearXNGWebSearchBackend(std::string base_url);
    [[nodiscard]] std::vector<WebSearchResult>
    search(std::string_view query, std::size_t max_results, std::chrono::milliseconds timeout,
           const BuiltinToolControl& control) const override;

private:
    std::string base_url_;
};

[[nodiscard]] std::vector<WebSearchResult> parse_searxng_search_response(std::string_view body,
                                                                         std::size_t max_results);

[[nodiscard]] std::shared_ptr<const BuiltinTool>
make_web_search_tool(std::shared_ptr<const WebSearchBackend> backend, std::size_t max_results,
                     std::chrono::milliseconds timeout);
[[nodiscard]] std::shared_ptr<const BuiltinTool>
make_web_open_tool(const BuiltinWebOptions& options);

[[nodiscard]] ToolDefinition web_search_tool_definition();
[[nodiscard]] ToolDefinition web_open_tool_definition();

class BuiltinToolRegistry {
public:
    BuiltinToolRegistry() = default;
    explicit BuiltinToolRegistry(const BuiltinWebOptions& options);
    explicit BuiltinToolRegistry(std::vector<std::shared_ptr<const BuiltinTool>> tools,
                                 std::uint32_t max_rounds = 4);

    [[nodiscard]] const BuiltinTool* find(std::string_view name) const noexcept;

    [[nodiscard]] bool enabled() const noexcept { return !tools_.empty(); }

    [[nodiscard]] std::uint32_t max_rounds() const noexcept { return max_rounds_; }

private:
    std::unordered_map<std::string, std::shared_ptr<const BuiltinTool>> tools_;
    std::uint32_t max_rounds_ = 0;
};

class ToolOrchestrator {
public:
    using GenerateNext = std::function<GenerationOutcome(const GenerationRequest&)>;

    explicit ToolOrchestrator(const BuiltinToolRegistry& registry) : registry_(&registry) {}

    [[nodiscard]] GenerationOutcome run(GenerationRequest request, GenerationOutcome first,
                                        std::optional<std::size_t> max_tool_calls,
                                        const BuiltinToolControl& control,
                                        const GenerateNext& generate_next) const;

private:
    const BuiltinToolRegistry* registry_;
};

} // namespace ninfer::serve
