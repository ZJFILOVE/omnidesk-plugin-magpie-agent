#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <omnidesk/async.hpp>
#include <string>

namespace magpie_agent {

struct HttpResponse {
    bool ok = false;
    long status_code = 0;
    std::string content_type;
    std::string body;
    std::string error;
    bool timed_out = false;
    bool canceled = false;
    bool connection_refused = false;
};

struct HttpRequestOptions {
    int connect_timeout_ms = 1500;
    int timeout_ms = 60000;
    std::string bearer_token;
    const std::atomic<bool>* cancel_flag = nullptr;
    std::shared_ptr<omnidesk::CancelToken> cancel_token;
    bool bypass_proxy = true;
};

using HttpStreamCallback = std::function<bool(const char* data,
                                              std::size_t size,
                                              const std::string& content_type)>;

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    virtual HttpResponse get(const std::string& url, const HttpRequestOptions& opts) = 0;
    virtual HttpResponse postJson(const std::string& url,
                                  const std::string& json_body,
                                  const HttpRequestOptions& opts) = 0;
    virtual HttpResponse postJsonStream(const std::string& url,
                                        const std::string& json_body,
                                        const HttpRequestOptions& opts,
                                        const HttpStreamCallback& on_chunk) {
        HttpResponse res = postJson(url, json_body, opts);
        if (on_chunk && !res.body.empty()) {
            on_chunk(res.body.data(), res.body.size(), res.content_type);
        }
        return res;
    }
};

class CurlHttpClient final : public IHttpClient {
public:
    CurlHttpClient() = default;
    ~CurlHttpClient() override = default;

    HttpResponse get(const std::string& url, const HttpRequestOptions& opts) override;
    HttpResponse postJson(const std::string& url,
                          const std::string& json_body,
                          const HttpRequestOptions& opts) override;
    HttpResponse postJsonStream(const std::string& url,
                                const std::string& json_body,
                                const HttpRequestOptions& opts,
                                const HttpStreamCallback& on_chunk) override;

private:
    HttpResponse execute(const std::string& url,
                         bool is_post,
                         const std::string& body,
                         const HttpRequestOptions& opts,
                         const HttpStreamCallback& on_chunk);
};

} // namespace magpie_agent
