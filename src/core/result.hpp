#pragma once

#include <string>
#include <variant>
#include <utility>

namespace alasia {

/// Error type for Result
struct Error {
    std::string message;
    
    Error() = default;
    explicit Error(std::string msg) : message(std::move(msg)) {}
    explicit Error(const char* msg) : message(msg) {}
};

/// Result type for error handling (alternative to std::expected)
template<typename T>
class Result {
public:
    // Success constructor
    Result(T value) : data_(std::move(value)) {}
    
    // Error constructor - accepts Error type
    Result(Error error) : data_(std::move(error)) {}
    
    // Error constructor - accepts const char* (convenience)
    Result(const char* error_msg) : data_(Error(error_msg)) {}
    
    // Check if result is ok
    [[nodiscard]] bool is_ok() const {
        return std::holds_alternative<T>(data_);
    }
    
    // Check if result is error
    [[nodiscard]] bool is_error() const {
        return std::holds_alternative<Error>(data_);
    }
    
    // Get value (undefined behavior if error)
    [[nodiscard]] const T& value() const {
        return std::get<T>(data_);
    }
    
    // Get value or default
    [[nodiscard]] T value_or(const T& default_value) const {
        if (is_ok()) {
            return std::get<T>(data_);
        }
        return default_value;
    }
    
    // Get error (undefined behavior if ok)
    [[nodiscard]] const Error& error() const {
        return std::get<Error>(data_);
    }
    
    // Implicit conversion to bool (true if ok)
    explicit operator bool() const {
        return is_ok();
    }
    
    // Get error message if error, empty string otherwise
    [[nodiscard]] std::string error_message() const {
        if (is_error()) {
            return std::get<Error>(data_).message;
        }
        return "";
    }
    
    // Create error result (static factory for clarity)
    static Result<T> error(std::string msg) {
        return Result<T>(Error(std::move(msg)));
    }

private:
    std::variant<T, Error> data_;
};

// Specialization for void (error-only result)
template<>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    Result(const char* error_msg) : error_(error_msg) {}
    
    [[nodiscard]] bool is_ok() const { return error_.message.empty(); }
    [[nodiscard]] bool is_error() const { return !error_.message.empty(); }
    [[nodiscard]] const Error& error() const { return error_; }
    [[nodiscard]] std::string error_message() const { return error_.message; }
    explicit operator bool() const { return is_ok(); }
    
    // Create error result (static factory for clarity)
    static Result<void> error(std::string msg) {
        return Result<void>(Error(std::move(msg)));
    }

private:
    Error error_;
};

} // namespace alasia
