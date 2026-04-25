#pragma once

#include "core/result.hpp"
#include <map>
#include <memory>
#include <string>

namespace alasia::http {

/// HTTP response
struct Response {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

/// HTTP request methods
enum class Method {
    GET,
    POST,
    PUT,
    DELETE
};

/// HTTP client interface
class HttpClient {
public:
    virtual ~HttpClient() = default;
    
    /// Send HTTP request
    virtual Result<Response> send(Method method, 
                                   const std::string& url,
                                   const std::string& body = "",
                                   const std::map<std::string, std::string>& headers = {}) = 0;
    
    /// GET request
    Result<Response> get(const std::string& url,
                         const std::map<std::string, std::string>& headers = {}) {
        return send(Method::GET, url, "", headers);
    }
    
    /// POST request
    Result<Response> post(const std::string& url,
                          const std::string& body,
                          const std::map<std::string, std::string>& headers = {}) {
        return send(Method::POST, url, body, headers);
    }
    
    /// Set proxy URL (empty to clear)
    virtual void set_proxy(const std::string& proxy_url) = 0;
};

/// Create default HTTP client instance
std::unique_ptr<HttpClient> create_default_client();

} // namespace alasia::http
