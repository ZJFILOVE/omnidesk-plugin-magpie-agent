#include "magpie_agent/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <mutex>

namespace magpie_agent {

namespace {

void ensureCurlInitialized() {
    static std::once_flag once;
    std::call_once(once, []() { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct TransferContext {
    const HttpRequestOptions* opts = nullptr;
    const HttpStreamCallback* on_chunk = nullptr;
    std::string* body = nullptr;
    std::string content_type;
    bool aborted_by_callback = false;
};

bool isRequestCanceled(const HttpRequestOptions* opts) {
    if (!opts) {
        return false;
    }
    if (opts->cancel_flag && opts->cancel_flag->load()) {
        return true;
    }
    if (opts->cancel_token && opts->cancel_token->isCancelled()) {
        return true;
    }
    return false;
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::size_t headerCallback(char* buffer, std::size_t size, std::size_t nitems, void* userdata) {
    const std::size_t total = size * nitems;
    auto* ctx = static_cast<TransferContext*>(userdata);
    std::string line(buffer, total);
    std::string lower = toLowerAscii(line);
    if (lower.rfind("content-type:", 0) == 0) {
        std::string val = line.substr(13);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
            val.pop_back();
        }
        ctx->content_type = toLowerAscii(val);
    }
    return total;
}

std::size_t writeBodyCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* ctx = static_cast<TransferContext*>(userdata);
    const std::size_t total = size * nmemb;
    if (isRequestCanceled(ctx->opts)) {
        ctx->aborted_by_callback = true;
        return 0;
    }
    if (ctx->on_chunk && *ctx->on_chunk) {
        if (!(*ctx->on_chunk)(ptr, total, ctx->content_type)) {
            ctx->aborted_by_callback = true;
            return 0;
        }
    }
    if (ctx->body) {
        ctx->body->append(ptr, total);
    }
    return total;
}

int progressCallback(void* clientp,
                     curl_off_t /*dltotal*/,
                     curl_off_t /*dlnow*/,
                     curl_off_t /*ultotal*/,
                     curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<TransferContext*>(clientp);
    if (isRequestCanceled(ctx->opts)) {
        ctx->aborted_by_callback = true;
        return 1;
    }
    return 0;
}

} // namespace

HttpResponse CurlHttpClient::get(const std::string& url, const HttpRequestOptions& opts) {
    return execute(url, false, "", opts, nullptr);
}

HttpResponse CurlHttpClient::postJson(const std::string& url,
                                      const std::string& json_body,
                                      const HttpRequestOptions& opts) {
    return execute(url, true, json_body, opts, nullptr);
}

HttpResponse CurlHttpClient::postJsonStream(const std::string& url,
                                            const std::string& json_body,
                                            const HttpRequestOptions& opts,
                                            const HttpStreamCallback& on_chunk) {
    return execute(url, true, json_body, opts, on_chunk);
}

HttpResponse CurlHttpClient::execute(const std::string& url,
                                     bool is_post,
                                     const std::string& body,
                                     const HttpRequestOptions& opts,
                                     const HttpStreamCallback& on_chunk) {
    HttpResponse res;
    if (isRequestCanceled(&opts)) {
        res.canceled = true;
        res.error = "request canceled";
        return res;
    }

    ensureCurlInitialized();
    CURL* curl = ::curl_easy_init();
    if (!curl) {
        res.error = "curl_easy_init failed";
        return res;
    }

    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, "Accept: application/json, text/event-stream");
    if (is_post) {
        headers = ::curl_slist_append(headers, "Content-Type: application/json");
    }
    if (!opts.bearer_token.empty()) {
        std::string auth = "Authorization: Bearer " + opts.bearer_token;
        headers = ::curl_slist_append(headers, auth.c_str());
    }

    TransferContext tctx;
    tctx.opts = &opts;
    tctx.on_chunk = on_chunk ? &on_chunk : nullptr;
    tctx.body = &res.body;

    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    ::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(opts.connect_timeout_ms));
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(opts.timeout_ms));
    ::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    ::curl_easy_setopt(curl, CURLOPT_HEADERDATA, &tctx);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &tctx);

    if (opts.bypass_proxy) {
        ::curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    }
    if (opts.cancel_flag || opts.cancel_token) {
        ::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        ::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        ::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &tctx);
    }
    if (is_post) {
        ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    const CURLcode code = ::curl_easy_perform(curl);
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status_code);
    res.content_type = tctx.content_type;

    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);

    if (code == CURLE_OK) {
        res.ok = (res.status_code >= 200 && res.status_code < 300);
        if (!res.ok) {
            res.error = "HTTP " + std::to_string(res.status_code);
        }
        return res;
    }

    if (tctx.aborted_by_callback || code == CURLE_ABORTED_BY_CALLBACK || isRequestCanceled(&opts)) {
        res.canceled = true;
        res.error = "request canceled";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        res.timed_out = true;
        res.error = "HTTP request timed out";
    } else if (code == CURLE_COULDNT_CONNECT) {
        res.connection_refused = true;
        res.error = ::curl_easy_strerror(code);
    } else {
        res.error = ::curl_easy_strerror(code);
    }
    return res;
}

} // namespace magpie_agent
