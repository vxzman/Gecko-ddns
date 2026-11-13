#pragma once

#include <curl/curl.h>

#include <memory>
#include <string_view>

/// libcurl handles are freed only through these deleters.
struct CurlDeleter {
    void operator()(CURL* handle) const noexcept {
        if (handle != nullptr) curl_easy_cleanup(handle);
    }
};

struct SlistDeleter {
    void operator()(curl_slist* list) const noexcept {
        if (list != nullptr) curl_slist_free_all(list);
    }
};

using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;
using SlistPtr = std::unique_ptr<curl_slist, SlistDeleter>;

/// curl_slist_append returns null without freeing the existing list.
/// Release ownership only after a non-null head is returned.
[[nodiscard]] inline bool append_slist(SlistPtr& list, std::string_view line) {
    const std::string owned{line};
    curl_slist* next = curl_slist_append(list.get(), owned.c_str());
    if (next == nullptr) return false;
    list.release();
    list.reset(next);
    return true;
}
