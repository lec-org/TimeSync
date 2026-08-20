#pragma once

#include <QString>

#include <QtGlobal>

#include <type_traits>
#include <utility>

namespace TimeSync {

enum class ErrorCode {
    None = 0,
    InvalidArgument,
    InvalidConfiguration,
    ConfigurationReadFailed,
    ConfigurationWriteFailed,
    NetworkUnavailable,
    ProtocolRejected,
    SourceTimeout,
    NoValidSource,
    Cancelled,
    Busy,
    PrivilegeMissing,
    PrivilegeAdjustmentFailed,
    ElevationCancelled,
    ElevationFailed,
    SystemTimeRejected,
    VerificationFailed,
    UncertainState,
    SchedulerAccessDenied,
    SchedulerMutationFailed,
    TaskNotFound,
    UnsupportedPlatform,
    InternalError,
};

struct Error {
    ErrorCode code = ErrorCode::None;
    QString detail;
    quint32 nativeCode = 0;
    bool uncertain = false;

    [[nodiscard]] bool isValid() const noexcept { return code != ErrorCode::None; }
};

enum class ExitCode : int {
    Success = 0,
    ArgumentOrConfiguration = 2,
    RemoteTimeFailure = 3,
    PermissionFailure = 4,
    VerificationFailure = 5,
    OtherFailure = 6,
};

[[nodiscard]] ExitCode exitCodeForError(ErrorCode code) noexcept;

template <typename T>
class Result final {
    static_assert(!std::is_void_v<T>);

public:
    Result(const Result &) = default;
    Result(Result &&) noexcept = default;
    Result &operator=(const Result &) = default;
    Result &operator=(Result &&) noexcept = default;
    ~Result() = default;

    static Result success(T value)
    {
        Result result;
        result.value_ = std::move(value);
        result.ok_ = true;
        return result;
    }

    static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        result.ok_ = false;
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const T &value() const & { return value_; }
    [[nodiscard]] T &value() & { return value_; }
    [[nodiscard]] T &&value() && { return std::move(value_); }
    [[nodiscard]] const Error &error() const noexcept { return error_; }

private:
    Result() = default;

    bool ok_ = false;
    T value_{};
    Error error_{};
};

template <>
class Result<void> final {
public:
    static Result success()
    {
        Result result;
        result.ok_ = true;
        return result;
    }

    static Result failure(Error error)
    {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const Error &error() const noexcept { return error_; }

private:
    bool ok_ = false;
    Error error_{};
};

} // namespace TimeSync
