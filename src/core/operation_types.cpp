#include "operation_types.h"

#include "../platform/windows_system_clock.h"

namespace TimeSync {

OperationFailure operationFailureForError(const ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::None:
        return OperationFailure::None;
    case ErrorCode::Busy:
        return OperationFailure::Busy;
    case ErrorCode::InvalidArgument:
    case ErrorCode::InvalidConfiguration:
    case ErrorCode::ConfigurationReadFailed:
    case ErrorCode::ConfigurationWriteFailed:
        return OperationFailure::InvalidConfiguration;
    case ErrorCode::PrivilegeMissing:
    case ErrorCode::PrivilegeAdjustmentFailed:
        return OperationFailure::PrivilegeMissing;
    case ErrorCode::ElevationCancelled:
        return OperationFailure::AuthorizationDenied;
    case ErrorCode::VerificationFailed:
        return OperationFailure::VerificationFailed;
    case ErrorCode::Cancelled:
        return OperationFailure::Cancelled;
    case ErrorCode::UnsupportedPlatform:
        return OperationFailure::UnsupportedPlatform;
    case ErrorCode::NetworkUnavailable:
    case ErrorCode::ProtocolRejected:
    case ErrorCode::SourceTimeout:
    case ErrorCode::NoValidSource:
        return OperationFailure::RemoteTimeUnavailable;
    case ErrorCode::SystemTimeRejected:
        return OperationFailure::SystemTimeRejected;
    case ErrorCode::UncertainState:
        return OperationFailure::UncertainState;
    case ErrorCode::SchedulerAccessDenied:
        return OperationFailure::AuthorizationDenied;
    case ErrorCode::SchedulerMutationFailed:
    case ErrorCode::TaskNotFound:
    case ErrorCode::ElevationFailed:
    case ErrorCode::InternalError:
        return OperationFailure::Other;
    }
    return OperationFailure::Other;
}

OperationFailure operationFailureForSystemTimeFailure(const int failureValue) noexcept
{
    const auto failure = static_cast<SystemTimeFailure>(failureValue);
    switch (failure) {
    case SystemTimeFailure::None:
        return OperationFailure::None;
    case SystemTimeFailure::InvalidInput:
        return OperationFailure::InvalidConfiguration;
    case SystemTimeFailure::PrivilegeMissing:
    case SystemTimeFailure::PrivilegeAdjustmentFailed:
        return OperationFailure::PrivilegeMissing;
    case SystemTimeFailure::ApiRejected:
    case SystemTimeFailure::AccessDenied:
        return OperationFailure::SystemTimeRejected;
    case SystemTimeFailure::VerificationFailed:
        return OperationFailure::VerificationFailed;
    case SystemTimeFailure::Cancelled:
        return OperationFailure::Cancelled;
    case SystemTimeFailure::UncertainState:
        return OperationFailure::UncertainState;
    case SystemTimeFailure::UnsupportedPlatform:
        return OperationFailure::UnsupportedPlatform;
    }
    return OperationFailure::Other;
}

} // namespace TimeSync
