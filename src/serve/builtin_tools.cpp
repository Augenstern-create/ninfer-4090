#include "serve/builtin_tools.h"
#include "serve/generation_service.h"

#include "product/media_acquire/acquire.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ninfer::serve {
namespace {

using Json           = nlohmann::json;
using MediaError     = product::media_acquire::Error;
using MediaErrorKind = product::media_acquire::ErrorKind;

[[noreturn]] void tool_error(int status, std::string code, std::string message) {
    ApiError error;
    error.status  = status;
    error.type    = status >= 500 ? "server_error" : "invalid_request_error";
    error.param   = "tools";
    error.code    = std::move(code);
    error.message = std::move(message);
    throw ApiException(std::move(error));
}

Json arguments_object(std::string_view encoded, std::string_view tool) {
    const Json parsed = Json::parse(encoded, nullptr, false);
    if (!parsed.is_object()) {
        tool_error(400, "invalid_builtin_tool_arguments",
                   std::string(tool) + " arguments must encode a JSON object");
    }
    return parsed;
}

std::string required_string(const Json& object, const char* field, std::string_view tool) {
    if (!object.contains(field) || !object.at(field).is_string() ||
        object.at(field).get_ref<const std::string&>().empty()) {
        tool_error(400, "invalid_builtin_tool_arguments",
                   std::string(tool) + " requires a non-empty string '" + field + "'");
    }
    return object.at(field).get<std::string>();
}

std::string percent_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4U]);
            out.push_back(hex[c & 0x0fU]);
        }
    }
    return out;
}

void replace_all(std::string& text, std::string_view from, std::string_view to) {
    for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos;) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
}

std::string html_to_text(std::string_view html) {
    std::string filtered(html);
    std::string lower(filtered);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string_view tag : {"script", "style", "noscript"}) {
        const std::string opener = "<" + std::string(tag);
        const std::string closer = "</" + std::string(tag) + ">";
        for (std::size_t begin = 0; (begin = lower.find(opener, begin)) != std::string::npos;) {
            const std::size_t end = lower.find(closer, begin + opener.size());
            const std::size_t count =
                end == std::string::npos ? lower.size() - begin : end + closer.size() - begin;
            filtered.erase(begin, count);
            lower.erase(begin, count);
        }
    }
    html = filtered;
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    bool space  = false;
    for (std::size_t i = 0; i < html.size(); ++i) {
        const char c = html[i];
        if (!in_tag && c == '<') {
            in_tag = true;
            space  = true;
            continue;
        }
        if (in_tag) {
            if (c == '>') { in_tag = false; }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
            continue;
        }
        if (space && !out.empty() && out.back() != ' ') { out.push_back(' '); }
        space = false;
        out.push_back(c);
    }
    replace_all(out, "&nbsp;", " ");
    replace_all(out, "&amp;", "&");
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#39;", "'");
    return out;
}

ApiException map_http_error(const MediaError& error, std::string_view operation) {
    ApiError api;
    api.param   = "tools";
    api.message = std::string(operation) + ": " + error.what();
    switch (error.kind()) {
    case MediaErrorKind::BudgetExceeded:
        api.status = 400;
        api.code   = "builtin_tool_response_too_large";
        break;
    case MediaErrorKind::RemoteTimeout:
    case MediaErrorKind::DeadlineExceeded:
        api.status = 504;
        api.type   = "server_error";
        api.code   = "builtin_tool_timeout";
        break;
    case MediaErrorKind::Cancelled:
        api.status = 499;
        api.type   = "request_cancelled";
        api.code   = "client_disconnected";
        break;
    case MediaErrorKind::RemoteUnavailable:
        api.status = 502;
        api.type   = "server_error";
        api.code   = "builtin_tool_fetch_failed";
        break;
    }
    return ApiException(std::move(api));
}

class WebSearchTool final : public BuiltinTool {
public:
    WebSearchTool(std::shared_ptr<const WebSearchBackend> backend, std::size_t max_results,
                  std::chrono::milliseconds timeout)
        : definition_(web_search_tool_definition()), backend_(std::move(backend)),
          max_results_(max_results), timeout_(timeout) {}

    const ToolDefinition& definition() const noexcept override { return definition_; }

    std::string execute(std::string_view arguments_json,
                        const BuiltinToolControl& control) const override {
        const std::string query = required_string(
            arguments_object(arguments_json, kWebSearchToolName), "query", kWebSearchToolName);
        const std::vector<WebSearchResult> results =
            backend_->search(query, max_results_, timeout_, control);
        Json encoded = Json::array();
        for (const WebSearchResult& result : results) {
            encoded.push_back(
                Json{{"title", result.title}, {"url", result.url}, {"snippet", result.snippet}});
        }
        return Json{{"query", query}, {"results", std::move(encoded)}}.dump();
    }

private:
    ToolDefinition definition_;
    std::shared_ptr<const WebSearchBackend> backend_;
    std::size_t max_results_;
    std::chrono::milliseconds timeout_;
};

class WebOpenTool final : public BuiltinTool {
public:
    explicit WebOpenTool(const BuiltinWebOptions& options)
        : definition_(web_open_tool_definition()), max_bytes_(options.open_max_bytes),
          allow_private_network_(options.allow_private_network),
          timeout_(options.search_timeout_ms) {}

    const ToolDefinition& definition() const noexcept override { return definition_; }

    std::string execute(std::string_view arguments_json,
                        const BuiltinToolControl& control) const override {
        const std::string url = required_string(arguments_object(arguments_json, kWebOpenToolName),
                                                "url", kWebOpenToolName);
        product::media_acquire::Policy policy;
        policy.max_bytes             = max_bytes_;
        policy.connect_timeout_ms    = static_cast<int>(std::min<std::uint32_t>(timeout_, 5'000));
        policy.timeout_ms            = static_cast<int>(timeout_);
        policy.allow_private_network = allow_private_network_;
        policy.is_cancelled          = control.is_cancelled;
        product::media_acquire::HttpResponse response;
        try {
            response = product::media_acquire::acquire_http(url, policy);
        } catch (const MediaError& error) {
            throw map_http_error(error, "web_open failed");
        } catch (const std::invalid_argument& error) {
            tool_error(400, "web_open_url_rejected", error.what());
        }

        std::string type = response.content_type;
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool html =
            type.starts_with("text/html") || type.starts_with("application/xhtml+xml");
        const bool text = type.starts_with("text/plain") || type.starts_with("application/json") ||
                          type.starts_with("text/");
        if (!html && !text) {
            tool_error(400, "web_open_content_type_not_supported",
                       "web_open supports textual HTTP responses, received '" +
                           response.content_type + "'");
        }
        const std::string bytes(reinterpret_cast<const char*>(response.bytes.data()),
                                response.bytes.size());
        return Json{{"url", response.final_url},
                    {"content_type", response.content_type},
                    {"text", html ? html_to_text(bytes) : bytes}}
            .dump();
    }

private:
    ToolDefinition definition_;
    std::size_t max_bytes_;
    bool allow_private_network_;
    std::uint32_t timeout_;
};

ChatTurn assistant_tool_turn(const GenerationOutcome& outcome, const std::vector<ToolCall>& calls) {
    ChatTurn turn;
    turn.role              = ChatRole::Assistant;
    turn.reasoning_content = outcome.reasoning;
    if (!outcome.text.empty()) {
        turn.content.push_back(
            ContentPart{.kind = ContentKind::Text, .text = outcome.text, .type_raw = "text"});
    }
    for (const auto& call : calls) { turn.tool_calls.push_back(call); }
    return turn;
}

std::string new_builtin_call_id() {
    static std::atomic<std::uint64_t> sequence{0};
    return "builtin_call_" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed) + 1);
}

ChatTurn tool_result_turn(const ToolCall& call, std::string result, bool is_error) {
    ChatTurn turn;
    turn.role                 = ChatRole::Tool;
    turn.tool_call_id         = call.id;
    turn.tool_result_name     = call.name;
    turn.tool_result_is_error = is_error;
    turn.content.push_back(
        ContentPart{.kind = ContentKind::Text, .text = std::move(result), .type_raw = "text"});
    return turn;
}

void accumulate_usage(GenerationOutcome& total, const GenerationOutcome& round) {
    total.prompt_tokens += round.prompt_tokens;
    total.completion_tokens += round.completion_tokens;
    total.reasoning_tokens += round.reasoning_tokens;
    total.metrics.prepare_seconds += round.metrics.prepare_seconds;
    total.metrics.vision_seconds += round.metrics.vision_seconds;
    total.metrics.prefill_seconds += round.metrics.prefill_seconds;
    total.metrics.decode_seconds += round.metrics.decode_seconds;
    total.metrics.prompt_wall_seconds += round.metrics.prompt_wall_seconds;
    total.metrics.generation_wall_seconds += round.metrics.generation_wall_seconds;
    total.metrics.total_seconds += round.metrics.total_seconds;
}

GenerationOutcome finalize_outcome(GenerationOutcome current, const GenerationOutcome& cumulative,
                                   std::vector<ChatTurn> history) {
    current.prompt_tokens                   = cumulative.prompt_tokens;
    current.completion_tokens               = cumulative.completion_tokens;
    current.reasoning_tokens                = cumulative.reasoning_tokens;
    current.metrics.prepare_seconds         = cumulative.metrics.prepare_seconds;
    current.metrics.vision_seconds          = cumulative.metrics.vision_seconds;
    current.metrics.prefill_seconds         = cumulative.metrics.prefill_seconds;
    current.metrics.decode_seconds          = cumulative.metrics.decode_seconds;
    current.metrics.prompt_wall_seconds     = cumulative.metrics.prompt_wall_seconds;
    current.metrics.generation_wall_seconds = cumulative.metrics.generation_wall_seconds;
    current.metrics.total_seconds           = cumulative.metrics.total_seconds;
    current.builtin_history                 = std::move(history);
    return current;
}

} // namespace

ToolDefinition web_search_tool_definition() {
    ToolDefinition tool;
    tool.name        = kWebSearchToolName;
    tool.description = "Search the web and return ranked result titles, URLs, and snippets.";
    tool.input_schema_json =
        R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"]})";
    tool.execution = ToolExecution::Builtin;
    return tool;
}

ToolDefinition web_open_tool_definition() {
    ToolDefinition tool;
    tool.name        = kWebOpenToolName;
    tool.description = "Open an HTTP(S) URL and return readable text without executing JavaScript.";
    tool.input_schema_json =
        R"({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})";
    tool.execution = ToolExecution::Builtin;
    return tool;
}

SearXNGWebSearchBackend::SearXNGWebSearchBackend(std::string base_url)
    : base_url_(std::move(base_url)) {
    while (!base_url_.empty() && base_url_.back() == '/') { base_url_.pop_back(); }
    if (base_url_.empty()) { throw std::invalid_argument("SearXNG URL must not be empty"); }
}

std::vector<WebSearchResult>
SearXNGWebSearchBackend::search(std::string_view query, std::size_t max_results,
                                std::chrono::milliseconds timeout,
                                const BuiltinToolControl& control) const {
    product::media_acquire::Policy policy;
    policy.max_bytes             = 4ULL << 20;
    policy.connect_timeout_ms    = static_cast<int>(std::min<std::int64_t>(timeout.count(), 5'000));
    policy.timeout_ms            = static_cast<int>(timeout.count());
    policy.allow_private_network = true; // startup-configured SearXNG is a trusted backend.
    policy.is_cancelled          = control.is_cancelled;
    product::media_acquire::HttpResponse response;
    try {
        response = product::media_acquire::acquire_http(
            base_url_ + "/search?q=" + percent_encode(query) + "&format=json", policy);
    } catch (const MediaError& error) {
        throw map_http_error(error, "web_search failed");
    } catch (const std::invalid_argument& error) {
        tool_error(502, "web_search_backend_invalid", error.what());
    }
    return parse_searxng_search_response(
        std::string_view(reinterpret_cast<const char*>(response.bytes.data()),
                         response.bytes.size()),
        max_results);
}

std::vector<WebSearchResult> parse_searxng_search_response(std::string_view encoded,
                                                           std::size_t max_results) {
    const Json body = Json::parse(encoded, nullptr, false);
    if (!body.is_object() || !body.contains("results") || !body.at("results").is_array()) {
        tool_error(502, "web_search_backend_invalid",
                   "SearXNG returned an invalid JSON search response");
    }
    std::vector<WebSearchResult> results;
    for (const Json& item : body.at("results")) {
        if (results.size() >= max_results) { break; }
        if (!item.is_object() || !item.contains("url") || !item.at("url").is_string()) { continue; }
        results.push_back(WebSearchResult{
            .title   = item.value("title", ""),
            .url     = item.at("url").get<std::string>(),
            .snippet = item.value("content", ""),
        });
    }
    return results;
}

std::shared_ptr<const BuiltinTool>
make_web_search_tool(std::shared_ptr<const WebSearchBackend> backend, std::size_t max_results,
                     std::chrono::milliseconds timeout) {
    if (!backend || max_results == 0 || timeout.count() <= 0) {
        throw std::invalid_argument("web_search tool configuration must be positive");
    }
    return std::make_shared<WebSearchTool>(std::move(backend), max_results, timeout);
}

std::shared_ptr<const BuiltinTool> make_web_open_tool(const BuiltinWebOptions& options) {
    return std::make_shared<WebOpenTool>(options);
}

BuiltinToolRegistry::BuiltinToolRegistry(const BuiltinWebOptions& options) {
    if (!options.enabled) { return; }
    if (options.max_tool_rounds == 0 || options.search_timeout_ms == 0 ||
        options.search_max_results == 0 || options.open_max_bytes == 0) {
        throw std::invalid_argument("built-in Web tool limits must be positive");
    }
    auto backend = std::make_shared<SearXNGWebSearchBackend>(options.searxng_url);
    std::vector<std::shared_ptr<const BuiltinTool>> tools;
    tools.push_back(make_web_search_tool(backend, options.search_max_results,
                                         std::chrono::milliseconds(options.search_timeout_ms)));
    tools.push_back(make_web_open_tool(options));
    *this = BuiltinToolRegistry(std::move(tools), options.max_tool_rounds);
}

BuiltinToolRegistry::BuiltinToolRegistry(std::vector<std::shared_ptr<const BuiltinTool>> tools,
                                         std::uint32_t max_rounds)
    : max_rounds_(max_rounds) {
    if (!tools.empty() && max_rounds == 0) {
        throw std::invalid_argument("built-in tool max rounds must be positive");
    }
    for (std::shared_ptr<const BuiltinTool>& tool : tools) {
        if (!tool) { throw std::invalid_argument("built-in tool must not be null"); }
        const std::string name = tool->definition().name;
        if (!tools_.emplace(name, std::move(tool)).second) {
            throw std::invalid_argument("duplicate built-in tool: " + name);
        }
    }
}

const BuiltinTool* BuiltinToolRegistry::find(std::string_view name) const noexcept {
    const auto found = tools_.find(std::string(name));
    return found == tools_.end() ? nullptr : found->second.get();
}

GenerationOutcome ToolOrchestrator::run(GenerationRequest request, GenerationOutcome current,
                                        std::optional<std::size_t> max_tool_calls,
                                        const BuiltinToolControl& control,
                                        const GenerateNext& generate_next) const {
    GenerationOutcome cumulative;
    std::vector<ChatTurn> history;
    std::unordered_set<std::string> enabled_names;
    for (const ToolDefinition& tool : request.tools) {
        if (tool.execution == ToolExecution::Builtin && registry_->find(tool.name) != nullptr) {
            enabled_names.insert(tool.name);
        }
    }
    std::size_t executed = 0;
    std::uint32_t rounds = 0;
    for (;;) {
        std::vector<ninfer::GeneratedToolCall> builtin;
        std::vector<ninfer::GeneratedToolCall> external;
        for (const auto& call : current.tool_calls) {
            if (enabled_names.contains(call.name)) {
                builtin.push_back(call);
            } else {
                external.push_back(call);
            }
        }
        if (builtin.empty()) {
            accumulate_usage(cumulative, current);
            return finalize_outcome(std::move(current), cumulative, std::move(history));
        }
        if (rounds >= registry_->max_rounds()) {
            tool_error(400, "builtin_tool_round_limit", "maximum built-in tool rounds exceeded");
        }
        if (max_tool_calls && executed + builtin.size() > *max_tool_calls) {
            tool_error(400, "max_tool_calls_exceeded",
                       "built-in tool execution would exceed max_tool_calls");
        }
        ++rounds;
        executed += builtin.size();
        accumulate_usage(cumulative, current);

        std::vector<ToolCall> internal_calls;
        internal_calls.reserve(builtin.size());
        for (std::size_t index = 0; index < builtin.size(); ++index) {
            internal_calls.push_back(ToolCall{
                .id             = new_builtin_call_id(),
                .name           = builtin[index].name,
                .arguments_json = builtin[index].arguments_json,
            });
        }
        ChatTurn assistant = assistant_tool_turn(current, internal_calls);
        request.messages.push_back(assistant);
        history.push_back(std::move(assistant));
        for (std::size_t index = 0; index < builtin.size(); ++index) {
            const auto& call = builtin[index];
            if (control.is_cancelled && control.is_cancelled()) {
                tool_error(499, "client_disconnected", "built-in tool execution was cancelled");
            }
            std::string result;
            bool is_error = false;
            try {
                result = registry_->find(call.name)->execute(call.arguments_json, control);
            } catch (const ApiException& exception) {
                if (exception.error().status == 499) { throw; }
                is_error = true;
                result =
                    Json{{"error", exception.error().code}, {"message", exception.error().message}}
                        .dump();
            }
            ChatTurn tool = tool_result_turn(internal_calls[index], std::move(result), is_error);
            request.messages.push_back(tool);
            history.push_back(std::move(tool));
        }

        if (!external.empty()) {
            current.tool_calls = std::move(external);
            return finalize_outcome(std::move(current), cumulative, std::move(history));
        }
        request.max_tokens = std::max(0, request.max_tokens - current.completion_tokens);
        if (request.max_tokens == 0) {
            current.tool_calls.clear();
            current.finish_reason = ninfer::FinishReason::OutputLimit;
            return finalize_outcome(std::move(current), cumulative, std::move(history));
        }
        current = generate_next(request);
    }
}

} // namespace ninfer::serve
