#include "result.h"

namespace TimeSync {

ExitCode exitCodeForError(const ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::None:
        return ExitCode::Success;
    case ErrorCode::InvalidArgument:
    case ErrorCode::InvalidConfiguration:
    case ErrorCode::ConfigurationReadFailed:
    case ErrorCode::ConfigurationWriteFailed:
        return ExitCode::ArgumentOrConfiguration;
    case ErrorCode::NetworkUnavailable:
    case ErrorCode::ProtocolRejected:
    case ErrorCode::SourceTimeout:
    case ErrorCode::NoValidSource:
        return ExitCode::RemoteTimeFailure;
    case ErrorCode::PrivilegeMissing:
    case ErrorCode::PrivilegeAdjustmentFailed:
    case ErrorCode::ElevationCancelled:
    case ErrorCode::ElevationFailed:
    case ErrorCode::SchedulerAccessDenied:
        return ExitCode::PermissionFailure;
    case ErrorCode::VerificationFailed:
        return ExitCode::VerificationFailure;
    case ErrorCode::Cancelled:
    case ErrorCode::Busy:
    case ErrorCode::SystemTimeRejected:
    case ErrorCode::UncertainState:
    case ErrorCode::SchedulerMutationFailed:
    case ErrorCode::TaskNotFound:
    case ErrorCode::UnsupportedPlatform:
    case ErrorCode::InternalError:
        return ExitCode::OtherFailure;
    }
    return ExitCode::OtherFailure;
}

} // namespace TimeSync
