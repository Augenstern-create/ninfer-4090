#include "serve/builtin_tools.h"
#include "serve/generation_service.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace ninfer::serve;

int check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
    return 0;
}

std::string api_code(const std::function<void()>& action) {
    try {
        action();
    } catch (const ApiException& exception) { return exception.error().code; }
    return {};
}

class FakeTool final : public BuiltinTool {
public:
    explicit FakeTool(std::string name, std::shared_ptr<int> calls) : calls_(std::move(calls)) {
        definition_.name              = std::move(name);
        definition_.description       = "test built-in";
        definition_.input_schema_json = R"({"type":"object"})";
        definition_.execution         = ToolExecution::Builtin;
    }

    const ToolDefinition& definition() const noexcept override { return definition_; }

    std::string execute(std::string_view arguments, const BuiltinToolControl&) const override {
        ++*calls_;
        return std::string("result:") + std::string(arguments);
    }

private:
    ToolDefinition definition_;
    std::shared_ptr<int> calls_;
};

class TimeoutSearchBackend final : public WebSearchBackend {
public:
    std::vector<WebSearchResult> search(std::string_view, std::size_t, std::chrono::milliseconds,
                                        const BuiltinToolControl&) const override {
        ApiError error;
        error.status  = 504;
        error.type    = "server_error";
        error.code    = "builtin_tool_timeout";
        error.message = "timeout";
        throw ApiException(std::move(error));
    }
};

GenerationOutcome call(std::string name = "builtin") {
    GenerationOutcome outcome;
    outcome.prompt_tokens     = 2;
    outcome.completion_tokens = 1;
    outcome.reasoning_tokens  = 1;
    outcome.metrics.total_seconds           = 10.0;
    outcome.metrics.prefix_cache_hit_tokens = 7;
    outcome.metrics.speculative_rounds      = 11;
    outcome.tool_calls.push_back(
        ninfer::GeneratedToolCall{.name = std::move(name), .arguments_json = R"({"x":1})"});
    return outcome;
}

GenerationRequest request_with(const ToolDefinition& definition) {
    GenerationRequest request;
    request.tools.push_back(definition);
    request.max_tokens = 32;
    return request;
}

int test_registration_and_schema() {
    int failures                = 0;
    const ToolDefinition search = web_search_tool_definition();
    failures += check(search.name == "web_search" && search.execution == ToolExecution::Builtin &&
                          search.input_schema_json.find("query") != std::string::npos,
                      "web_search schema and built-in ownership are explicit");
    const ToolDefinition open = web_open_tool_definition();
    failures +=
        check(open.name == "web_open" && open.input_schema_json.find("url") != std::string::npos,
              "web_open schema is registered");
    auto calls              = std::make_shared<int>(0);
    bool duplicate_rejected = false;
    try {
        (void)BuiltinToolRegistry({std::make_shared<FakeTool>("duplicate", calls),
                                   std::make_shared<FakeTool>("duplicate", calls)},
                                  1);
    } catch (const std::invalid_argument&) { duplicate_rejected = true; }
    failures += check(duplicate_rejected, "built-in registry rejects duplicate identities");

    const auto parsed = parse_searxng_search_response(
        R"({"results":[{"title":"A","url":"https://a.example","content":"alpha"},{"title":"B","url":"https://b.example","content":"beta"}]})",
        1);
    failures += check(parsed.size() == 1 && parsed[0].title == "A" && parsed[0].snippet == "alpha",
                      "SearXNG JSON parsing preserves and bounds results");
    failures += check(api_code([] { (void)parse_searxng_search_response("{}", 2); }) ==
                          "web_search_backend_invalid",
                      "malformed SearXNG response has a stable backend error");
    const auto timeout = make_web_search_tool(std::make_shared<TimeoutSearchBackend>(), 2,
                                              std::chrono::milliseconds(10));
    failures += check(api_code([&] { (void)timeout->execute(R"({"query":"x"})", {}); }) ==
                          "builtin_tool_timeout",
                      "web_search backend timeout retains a stable error");
    return failures;
}

int test_orchestration() {
    int failures = 0;
    auto calls   = std::make_shared<int>(0);
    auto tool    = std::make_shared<FakeTool>("builtin", calls);
    BuiltinToolRegistry registry({tool}, 2);
    ToolOrchestrator orchestrator(registry);
    GenerationRequest request = request_with(tool->definition());

    int generated            = 0;
    GenerationOutcome result = orchestrator.run(
        request, call(), std::nullopt, {}, [&](const GenerationRequest& continuation) {
            ++generated;
            failures +=
                check(continuation.messages.size() == static_cast<std::size_t>(generated * 2) &&
                          continuation.messages[0].role == ninfer::ChatRole::Assistant &&
                          continuation.messages[1].role == ninfer::ChatRole::Tool,
                      "built-in call and result enter continuation history");
            if (generated == 1) {
                GenerationOutcome next = call();
                next.prompt_tokens      = 3;
                next.completion_tokens  = 2;
                return next;
            }
            GenerationOutcome final;
            final.text                            = "done";
            final.prompt_tokens                   = 4;
            final.completion_tokens               = 2;
            final.reasoning_tokens                = 1;
            final.metrics.total_seconds           = 20.0;
            final.metrics.prefix_cache_hit_tokens = 3;
            final.metrics.speculative_rounds      = 5;
            return final;
        });
    failures += check(*calls == 2 && generated == 2 && result.text == "done" &&
                          result.builtin_history.size() == 4 && result.prompt_tokens == 9 &&
                          result.completion_tokens == 5 && result.reasoning_tokens == 3,
                      "multi-round built-in continuation accumulates all token usage");
    failures += check(result.metrics.total_seconds == 20.0 &&
                          result.metrics.prefix_cache_hit_tokens == 3 &&
                          result.metrics.speculative_rounds == 5,
                      "continuation metrics consistently describe only the terminal round");

    *calls                     = 0;
    GenerationOutcome external = call("external");
    result                     = orchestrator.run(request, std::move(external), std::nullopt, {},
                                                  [&](const GenerationRequest&) {
                                  ++generated;
                                  return GenerationOutcome{};
                                                  });
    failures += check(*calls == 0 && result.tool_calls.size() == 1 &&
                          result.tool_calls[0].name == "external",
                      "external tool calls are not intercepted");

    *calls = 0;
    result = orchestrator.run(request, call(), std::nullopt, {},
                              [&](const GenerationRequest& continuation) {
                                  failures += check(continuation.messages.size() == 2,
                                                    "built-in history precedes external call");
                                  return call("external");
                              });
    failures += check(*calls == 1 && result.tool_calls.size() == 1 &&
                          result.tool_calls[0].name == "external" &&
                          result.builtin_history.size() == 2 && result.prompt_tokens == 4 &&
                          result.completion_tokens == 2 && result.reasoning_tokens == 2,
                      "external call after a completed built-in round retains coherent history");

    *calls                  = 0;
    GenerationOutcome mixed = call();
    mixed.tool_calls.push_back(
        ninfer::GeneratedToolCall{.name = "external", .arguments_json = "{}"});
    bool mixed_generated = false;
    failures += check(
        api_code([&] {
            (void)orchestrator.run(request, std::move(mixed), std::nullopt, {},
                                   [&](const GenerationRequest&) {
                                       mixed_generated = true;
                                       return GenerationOutcome{};
                                   });
        }) == "mixed_builtin_external_tool_calls_not_supported" &&
            *calls == 0 && !mixed_generated,
        "mixed built-in and external calls are rejected before any side effect");

    BuiltinToolRegistry one_round({tool}, 1);
    ToolOrchestrator bounded(one_round);
    failures += check(api_code([&] {
                          (void)bounded.run(request, call(), std::nullopt, {},
                                            [&](const GenerationRequest&) { return call(); });
                      }) == "builtin_tool_round_limit",
                      "maximum built-in round count terminates a loop");
    failures +=
        check(api_code([&] {
                  (void)orchestrator.run(request, call(), 0, {}, [&](const GenerationRequest&) {
                      return GenerationOutcome{};
                  });
              }) == "max_tool_calls_exceeded",
              "request max_tool_calls bounds built-in execution");
    return failures;
}

class LocalServer {
public:
    LocalServer() {
        server_.Get("/html", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(
                "<html><!-- hidden --><script>ignore()</script><body><h1>Hello &amp; world</h1>"
                "<p>First<br>Second</p><ul><li>one</li><li>two</li></ul>"
                "<pre>code  block\nnext</pre></body></html>",
                "text/html");
        });
        server_.Get("/large", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(std::string(4096, 'x'), "text/plain");
        });
        server_.Get("/redirect", [this](const httplib::Request&, httplib::Response& response) {
            response.set_redirect(url("/html"));
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) { throw std::runtime_error("failed to bind local test server"); }
        thread_ = std::thread([this] { server_.listen_after_bind(); });
    }

    ~LocalServer() {
        server_.stop();
        if (thread_.joinable()) { thread_.join(); }
    }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

private:
    httplib::Server server_;
    int port_ = -1;
    std::thread thread_;
};

int test_web_open() {
    LocalServer server;
    BuiltinWebOptions options;
    options.open_max_bytes        = 1024;
    options.search_timeout_ms     = 1;
    options.open_timeout_ms       = 2'000;
    options.allow_private_network = true;
    const auto open               = make_web_open_tool(options);
    const std::string result =
        open->execute(std::string("{\"url\":\"") + server.url("/html") + "\"}", {});
    const std::string text = nlohmann::json::parse(result).at("text").get<std::string>();
    int failures = check(text.find("Hello & world") != std::string::npos &&
                             text.find("First\nSecond") != std::string::npos &&
                             text.find("- one") != std::string::npos &&
                             text.find("code  block\nnext") != std::string::npos &&
                             text.find("ignore") == std::string::npos &&
                             text.find("hidden") == std::string::npos,
                         "web_open performs bounded basic HTML text stripping");
    const std::string redirected =
        open->execute(std::string("{\"url\":\"") + server.url("/redirect") + "\"}", {});
    failures += check(redirected.find("Hello & world") != std::string::npos,
                      "web_open follows checked HTTP redirects");

    options.allow_private_network = false;
    const auto protected_open     = make_web_open_tool(options);
    failures += check(api_code([&] {
                          (void)protected_open->execute(
                              std::string("{\"url\":\"") + server.url("/html") + "\"}", {});
                      }) == "web_open_url_rejected",
                      "web_open rejects loopback/private destinations by default");
    failures += check(api_code([&] {
                          (void)open->execute(
                              std::string("{\"url\":\"") + server.url("/large") + "\"}", {});
                      }) == "builtin_tool_response_too_large",
                      "web_open enforces its response byte limit");
    failures += check(api_code([&] {
                          (void)open->execute(R"({"url":"file:///etc/passwd"})", {});
                      }) == "web_open_url_rejected",
                      "web_open rejects non-HTTP schemes");
    return failures;
}

} // namespace

int main() {
    int failures = 0;
    failures += test_registration_and_schema();
    failures += test_orchestration();
    failures += test_web_open();
    if (failures == 0) { std::cout << "builtin tool tests passed\n"; }
    return failures == 0 ? 0 : 1;
}
