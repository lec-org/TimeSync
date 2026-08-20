#pragma once

#include "../core/result.h"

#include <QMutex>

namespace TimeSync {

class CrossProcessMutex final {
public:
    static constexpr int DefaultWaitMs = 250;

    CrossProcessMutex();
    ~CrossProcessMutex();

    CrossProcessMutex(const CrossProcessMutex &) = delete;
    CrossProcessMutex &operator=(const CrossProcessMutex &) = delete;

    [[nodiscard]] Result<void> acquire(int timeoutMs = DefaultWaitMs);
    void release();
    [[nodiscard]] bool isOwned() const noexcept { return owned_; }

private:
#ifdef Q_OS_WIN
    void *handle_ = nullptr;
#else
    QMutex processMutex_;
#endif
    bool owned_ = false;
};

} // namespace TimeSync
