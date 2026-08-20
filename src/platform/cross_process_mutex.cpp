#include "cross_process_mutex.h"

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace TimeSync {

CrossProcessMutex::CrossProcessMutex() = default;

CrossProcessMutex::~CrossProcessMutex()
{
    release();
#ifdef Q_OS_WIN
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#endif
}

Result<void> CrossProcessMutex::acquire(const int timeoutMs)
{
    if (owned_) {
        return Result<void>::failure({ErrorCode::Busy, QStringLiteral("mutex already owned")});
    }

#ifdef Q_OS_WIN
    if (handle_ == nullptr) {
        constexpr wchar_t MutexName[] = L"Local\\TimeSync.SystemTimeMutation.v1";
        handle_ = static_cast<void *>(CreateMutexW(nullptr, FALSE, MutexName));
        if (handle_ == nullptr) {
            return Result<void>::failure(
                {ErrorCode::InternalError, QStringLiteral("cannot create system-time mutex"), GetLastError()});
        }
    }

    const DWORD waitMs = timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs);
    const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(handle_), waitMs);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        owned_ = true;
        return Result<void>::success();
    }
    if (waitResult == WAIT_TIMEOUT) {
        return Result<void>::failure({ErrorCode::Busy, QStringLiteral("system-time mutex is busy"), WAIT_TIMEOUT});
    }
    return Result<void>::failure(
        {ErrorCode::InternalError, QStringLiteral("system-time mutex wait failed"), GetLastError()});
#else
    if (timeoutMs < 0) {
        processMutex_.lock();
        owned_ = true;
        return Result<void>::success();
    }
    if (!processMutex_.tryLock(timeoutMs)) {
        return Result<void>::failure({ErrorCode::Busy, QStringLiteral("system-time mutex is busy")});
    }
    owned_ = true;
    return Result<void>::success();
#endif
}

void CrossProcessMutex::release()
{
    if (!owned_) {
        return;
    }
#ifdef Q_OS_WIN
    if (handle_ != nullptr) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
    }
#else
    processMutex_.unlock();
#endif
    owned_ = false;
}

} // namespace TimeSync
