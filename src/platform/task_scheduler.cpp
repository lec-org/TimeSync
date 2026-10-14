#include "task_scheduler.h"

#include "elevation_broker.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <taskschd.h>
#    include <oleauto.h>
#    include <sddl.h>
#    include <shellapi.h>
#endif

namespace TimeSync {

#ifdef Q_OS_WIN
// Keep this COM interface externally linked, just like the SDK interfaces. An
// anonymous-namespace abstract type lets GCC infer that it has no implementations
// and replace the external COM virtual call with __cxa_pure_virtual at -O2/-O3.
// MinGW 13.1 omits ILogonTrigger; its ABI below matches Windows SDK 10.0.22621.0.
struct SchedulerLogonTrigger : ITrigger {
    virtual HRESULT STDMETHODCALLTYPE get_Delay(BSTR *delay) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Delay(BSTR delay) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UserId(BSTR *user) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserId(BSTR user) = 0;
};
#endif

namespace {

Error schedulerError(const ErrorCode code, const QString &detail, const quint32 nativeCode = 0)
{
    return {code, detail, nativeCode};
}

#ifdef Q_OS_WIN
// A local IID value also avoids depending on older libuuid exporting IID_ILogonTrigger.
constexpr IID LogonTriggerIid{0x72dade38, 0xfae4, 0x4b3e, {0xba, 0xf4, 0x5d, 0x00, 0x9a, 0xf0, 0x2b, 0x1c}};

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr()
    {
        if (pointer_ != nullptr) {
            pointer_->Release();
        }
    }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&other) noexcept
        : pointer_(other.pointer_)
    {
        other.pointer_ = nullptr;
    }
    ComPtr &operator=(ComPtr &&other) noexcept
    {
        if (this != &other) {
            if (pointer_ != nullptr) {
                pointer_->Release();
            }
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }
    T **out()
    {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
        return &pointer_;
    }
    T *operator->() const { return pointer_; }
    T *get() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }
    void reset()
    {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T *pointer_ = nullptr;
};

class ScopedBstr final {
public:
    explicit ScopedBstr(const std::wstring &value)
        : value_(SysAllocString(value.c_str()))
    {
    }
    ~ScopedBstr()
    {
        if (value_ != nullptr) {
            SysFreeString(value_);
        }
    }
    BSTR get() const { return value_; }

private:
    BSTR value_ = nullptr;
};

class ScopedVariant final {
public:
    ScopedVariant() { VariantInit(&value_); }
    ~ScopedVariant() { VariantClear(&value_); }
    VARIANT *get() { return &value_; }
    operator VARIANT() const { return value_; }

private:
    VARIANT value_{};
};

class ComApartmentGuard final {
public:
    void setActive(const bool active) { active_ = active; }
    ~ComApartmentGuard()
    {
        if (active_) {
            CoUninitialize();
        }
    }

private:
    bool active_ = false;
};

Result<void> ensureElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        const DWORD error = GetLastError();
        return Result<void>::failure(schedulerError(ErrorCode::SchedulerAccessDenied,
                                                    QStringLiteral("cannot inspect task-management token"),
                                                    error));
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL queried = GetTokenInformation(token,
                                             TokenElevation,
                                             &elevation,
                                             sizeof(elevation),
                                             &returned);
    const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    if (!queried || elevation.TokenIsElevated == 0) {
        return Result<void>::failure(schedulerError(ErrorCode::SchedulerAccessDenied,
                                                    QStringLiteral("task mutation requires elevation"),
                                                    error));
    }
    return Result<void>::success();
}

Result<ComPtr<ITaskService>> connectService(bool *uninitialize)
{
    *uninitialize = false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return Result<ComPtr<ITaskService>>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("task scheduler COM initialization failed"),
                           static_cast<quint32>(initialized)));
    }
    *uninitialize = SUCCEEDED(initialized);

    ComPtr<ITaskService> service;
    const HRESULT created = CoCreateInstance(CLSID_TaskScheduler,
                                              nullptr,
                                              CLSCTX_INPROC_SERVER,
                                              IID_ITaskService,
                                              reinterpret_cast<void **>(service.out()));
    if (FAILED(created)) {
        if (*uninitialize) {
            CoUninitialize();
        }
        return Result<ComPtr<ITaskService>>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("task scheduler service unavailable"),
                           static_cast<quint32>(created)));
    }

    ScopedVariant empty;
    const HRESULT connected = service->Connect(*empty.get(), *empty.get(), *empty.get(), *empty.get());
    if (FAILED(connected)) {
        service.reset();
        if (*uninitialize) {
            CoUninitialize();
        }
        return Result<ComPtr<ITaskService>>::failure(
            schedulerError(connected == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                       : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("task scheduler connection failed"),
                           static_cast<quint32>(connected)));
    }
    return Result<ComPtr<ITaskService>>::success(std::move(service));
}

Result<ComPtr<ITaskFolder>> rootFolder(ITaskService *service)
{
    ScopedBstr rootName(L"\\");
    ComPtr<ITaskFolder> root;
    const HRESULT result = service->GetFolder(rootName.get(), root.out());
    if (FAILED(result)) {
        return Result<ComPtr<ITaskFolder>>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("task scheduler root unavailable"),
                           static_cast<quint32>(result)));
    }
    return Result<ComPtr<ITaskFolder>>::success(std::move(root));
}

QString bstrToQString(BSTR value)
{
    if (value == nullptr) {
        return {};
    }
    const QString result = QString::fromWCharArray(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

Result<QString> currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Result<QString>::failure(schedulerError(ErrorCode::SchedulerAccessDenied,
                                                       QStringLiteral("cannot inspect current user"), GetLastError()));
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<qsizetype>(size), Qt::Uninitialized);
    const BOOL queried = size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    const DWORD error = queried ? ERROR_SUCCESS : GetLastError();
    CloseHandle(token);
    LPWSTR sid = nullptr;
    if (!queried || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid)) {
        return Result<QString>::failure(schedulerError(ErrorCode::SchedulerAccessDenied,
                                                       QStringLiteral("cannot identify current user"),
                                                       queried ? GetLastError() : error));
    }
    const QString value = QString::fromWCharArray(sid);
    LocalFree(sid);
    return Result<QString>::success(value);
}

bool identifiesUser(const QString &user, const QString &sid)
{
    if (user.compare(sid, Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (user.isEmpty()) {
        return false;
    }
    // Existing periodic tasks can store DOMAIN\\user rather than its SID.
    DWORD sidSize = 0;
    DWORD domainSize = 0;
    SID_NAME_USE use{};
    const std::wstring account = user.toStdWString();
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidSize, nullptr, &domainSize, &use);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sidSize == 0) {
        return false;
    }
    QByteArray sidBuffer(static_cast<qsizetype>(sidSize), Qt::Uninitialized);
    std::wstring domain(domainSize, L'\0');
    if (!LookupAccountNameW(nullptr, account.c_str(), sidBuffer.data(), &sidSize,
                            domain.data(), &domainSize, &use)) {
        return false;
    }
    LPWSTR resolvedSid = nullptr;
    if (!ConvertSidToStringSidW(sidBuffer.data(), &resolvedSid)) {
        return false;
    }
    const bool matches = QString::fromWCharArray(resolvedSid).compare(sid, Qt::CaseInsensitive) == 0;
    LocalFree(resolvedSid);
    return matches;
}

bool sameExecutable(const QString &left, const QString &right)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(left).absoluteFilePath()))
        .compare(QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(right).absoluteFilePath())),
                 Qt::CaseInsensitive) == 0;
}

QStringList actionArgumentValues(const QString &arguments)
{
    // Parse semantic identity with Windows argv rules, without imposing canonical quoting/order.
    const std::wstring commandLine = (QStringLiteral("TimeSync.exe ") + arguments).toStdWString();
    int count = 0;
    LPWSTR *argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (argv == nullptr) {
        return {};
    }
    QStringList values;
    for (int index = 1; index < count; ++index) {
        values.append(QString::fromWCharArray(argv[index]));
    }
    LocalFree(argv);
    return values;
}

Result<ScheduledTaskInfo> queryTask(ITaskFolder *root, const ScheduledTaskKind kind,
                                   const bool validateTemplate = true)
{
    ScopedBstr name(TaskSchedulerBackend::taskName(kind).toStdWString());
    ComPtr<IRegisteredTask> task;
    const HRESULT found = root->GetTask(name.get(), task.out());
    if (FAILED(found)) {
        if (found == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return Result<ScheduledTaskInfo>::success({});
        }
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(found == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                   : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("cannot query scheduled task"),
                           static_cast<quint32>(found)));
    }

    ScheduledTaskInfo info;
    info.exists = true;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query task state")));
    }
    info.enabled = enabled == VARIANT_TRUE;

    ComPtr<ITaskDefinition> definition;
    HRESULT result = task->get_Definition(definition.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query task definition"), result));
    }
    const auto invalidDefinition = [] {
        return Result<ScheduledTaskInfo>::failure(schedulerError(
            ErrorCode::SchedulerMutationFailed,
            QStringLiteral("task name is occupied by an unrelated or invalid task; refusing to modify it")));
    };
    ComPtr<IRegistrationInfo> registration;
    BSTR author = nullptr;
    if (FAILED(definition->get_RegistrationInfo(registration.out()))
        || FAILED(registration->get_Author(&author))) {
        return invalidDefinition();
    }
    if (bstrToQString(author) != QStringLiteral("TimeSync")) {
        return invalidDefinition();
    }
    const Result<QString> userSid = currentUserSid();
    if (!userSid) {
        return Result<ScheduledTaskInfo>::failure(userSid.error());
    }
    ComPtr<IPrincipal> principal;
    TASK_LOGON_TYPE logonType{};
    TASK_RUNLEVEL_TYPE runLevel{};
    BSTR principalUser = nullptr;
    if (FAILED(definition->get_Principal(principal.out()))
        || FAILED(principal->get_UserId(&principalUser))) {
        return invalidDefinition();
    }
    const QString principalId = bstrToQString(principalUser);
    if (!identifiesUser(principalId, userSid.value())) {
        return invalidDefinition();
    }
    if (kind == ScheduledTaskKind::Startup && validateTemplate
        && (FAILED(principal->get_LogonType(&logonType))
            || FAILED(principal->get_RunLevel(&runLevel))
            || logonType != TASK_LOGON_INTERACTIVE_TOKEN || runLevel != TASK_RUNLEVEL_HIGHEST)) {
        return invalidDefinition();
    }
    ComPtr<IActionCollection> actions;
    result = definition->get_Actions(actions.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query task action"), result));
    }
    LONG actionCount = 0;
    if (FAILED(actions->get_Count(&actionCount)) || actionCount != 1) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("task must have exactly one action")));
    }
    ComPtr<IAction> action;
    result = actions->get_Item(1, action.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query task action item"), result));
    }
    ComPtr<IExecAction> exec;
    result = action->QueryInterface(IID_IExecAction, reinterpret_cast<void **>(exec.out()));
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("task action is not executable"), result));
    }
    BSTR path = nullptr;
    BSTR arguments = nullptr;
    result = exec->get_Path(&path);
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query executable path"), result));
    }
    info.executablePath = bstrToQString(path);
    if (FAILED(exec->get_Arguments(&arguments))) {
        return invalidDefinition();
    }
    info.arguments = bstrToQString(arguments);
    const QStringList argumentValues = actionArgumentValues(info.arguments);
    if (!TaskSchedulerBackend::isOwnedTaskAction(info.executablePath, argumentValues, kind)) {
        return invalidDefinition();
    }
    // A missing/moved executable or noncanonical but recognizable action does not erase
    // ownership. Allow removal/reinstallation of our task, even when its template needs repair.
    if (kind == ScheduledTaskKind::Startup && !validateTemplate) {
        return Result<ScheduledTaskInfo>::success(std::move(info));
    }
    if (kind == ScheduledTaskKind::Periodic) {
        // Preserve the original periodic query contract: inspect the first time trigger,
        // without imposing Startup's trigger/settings/canonical-action requirements.
        ComPtr<ITriggerCollection> triggers;
        LONG count = 0;
        ComPtr<ITrigger> trigger;
        ComPtr<ITimeTrigger> timeTrigger;
        ComPtr<IRepetitionPattern> repetition;
        BSTR interval = nullptr;
        if (SUCCEEDED(definition->get_Triggers(triggers.out()))
            && SUCCEEDED(triggers->get_Count(&count)) && count > 0
            && SUCCEEDED(triggers->get_Item(1, trigger.out()))
            && SUCCEEDED(trigger->QueryInterface(IID_ITimeTrigger, reinterpret_cast<void **>(timeTrigger.out())))
            && SUCCEEDED(timeTrigger->get_Repetition(repetition.out()))
            && SUCCEEDED(repetition->get_Interval(&interval))) {
            info.intervalMinutes = TaskSchedulerBackend::parseRepetitionIntervalMinutes(bstrToQString(interval));
        }
        return Result<ScheduledTaskInfo>::success(std::move(info));
    }
    BSTR workingDirectory = nullptr;
    if (FAILED(exec->get_WorkingDirectory(&workingDirectory))) {
        return invalidDefinition();
    }
    const QString directory = bstrToQString(workingDirectory);
    const int configIndex = argumentValues.indexOf(QStringLiteral("--config"));
    if (!directory.isEmpty()
        || info.arguments != TaskSchedulerBackend::buildActionArguments(argumentValues.at(configIndex + 1), kind)) {
        return invalidDefinition();
    }

    ComPtr<ITriggerCollection> triggers;
    LONG triggerCount = 0;
    ComPtr<ITrigger> trigger;
    TASK_TRIGGER_TYPE2 triggerType{};
    VARIANT_BOOL triggerEnabled = VARIANT_FALSE;
    if (FAILED(definition->get_Triggers(triggers.out()))
        || FAILED(triggers->get_Count(&triggerCount)) || triggerCount != 1
        || FAILED(triggers->get_Item(1, trigger.out()))
        || FAILED(trigger->get_Type(&triggerType))
        || FAILED(trigger->get_Enabled(&triggerEnabled)) || triggerEnabled != VARIANT_TRUE
        || triggerType != (kind == ScheduledTaskKind::Startup ? TASK_TRIGGER_LOGON : TASK_TRIGGER_TIME)) {
        return invalidDefinition();
    }
    ComPtr<IRepetitionPattern> repetition;
    BSTR interval = nullptr;
    if (FAILED(trigger->get_Repetition(repetition.out())) || FAILED(repetition->get_Interval(&interval))) {
        return invalidDefinition();
    }
    const QString intervalText = bstrToQString(interval);
    if (kind == ScheduledTaskKind::Startup) {
        BSTR duration = nullptr;
        BSTR startBoundary = nullptr;
        BSTR endBoundary = nullptr;
        if (FAILED(repetition->get_Duration(&duration))) {
            return invalidDefinition();
        }
        const QString durationText = bstrToQString(duration);
        if (FAILED(trigger->get_StartBoundary(&startBoundary))) {
            return invalidDefinition();
        }
        const QString start = bstrToQString(startBoundary);
        if (FAILED(trigger->get_EndBoundary(&endBoundary))) {
            return invalidDefinition();
        }
        const QString end = bstrToQString(endBoundary);
        ComPtr<SchedulerLogonTrigger> logon;
        BSTR delay = nullptr;
        BSTR triggerUser = nullptr;
        if (FAILED(trigger->QueryInterface(LogonTriggerIid, reinterpret_cast<void **>(logon.out())))
            || FAILED(logon->get_Delay(&delay))) {
            return invalidDefinition();
        }
        const QString delayText = bstrToQString(delay);
        if (FAILED(logon->get_UserId(&triggerUser))) {
            return invalidDefinition();
        }
        const QString triggerId = bstrToQString(triggerUser);
        if (!intervalText.isEmpty() || !durationText.isEmpty() || !start.isEmpty() || !end.isEmpty()
            || delayText != QStringLiteral("PT30S")
            || !identifiesUser(triggerId, userSid.value())) {
            return invalidDefinition();
        }
        ComPtr<ITaskSettings> settings;
        BSTR restartInterval = nullptr;
        int restartCount = 0;
        TASK_INSTANCES_POLICY instances{};
        if (FAILED(definition->get_Settings(settings.out()))
            || FAILED(settings->get_RestartCount(&restartCount))
            || FAILED(settings->get_RestartInterval(&restartInterval))) {
            return invalidDefinition();
        }
        const QString restart = bstrToQString(restartInterval);
        if (restartCount != 0 || !restart.isEmpty()
            || FAILED(settings->get_MultipleInstances(&instances)) || instances != TASK_INSTANCES_IGNORE_NEW) {
            return invalidDefinition();
        }
    } else {
        info.intervalMinutes = TaskSchedulerBackend::parseRepetitionIntervalMinutes(intervalText);
        if (info.intervalMinutes == 0) {
            return invalidDefinition();
        }
    }
    return Result<ScheduledTaskInfo>::success(std::move(info));
}

#endif

} // namespace

bool TaskSchedulerBackend::isOwnedTaskAction(const QString &executablePath,
                                             const QStringList &arguments,
                                             const ScheduledTaskKind kind)
{
    if (!QFileInfo(executablePath).isAbsolute()) {
        return false;
    }
    bool syncOnce = false;
    bool scheduled = false;
    bool startup = false;
    bool config = false;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        if (argument == QStringLiteral("--sync-once") && !syncOnce) {
            syncOnce = true;
        } else if (argument == QStringLiteral("--scheduled") && !scheduled) {
            scheduled = true;
        } else if (argument == QStringLiteral("--startup") && !startup) {
            startup = true;
        } else if (argument == QStringLiteral("--config") && !config
                   && index + 1 < arguments.size() && QFileInfo(arguments.at(index + 1)).isAbsolute()) {
            config = true;
            ++index;
        } else {
            return false;
        }
    }
    return syncOnce && scheduled && config && startup == (kind == ScheduledTaskKind::Startup);
}

QString TaskSchedulerBackend::taskName(const ScheduledTaskKind kind)
{
    if (kind == ScheduledTaskKind::Startup) {
        return QStringLiteral("TimeSync.StandardClock.StartupSync.v1");
    }
    return QStringLiteral("TimeSync.StandardClock.AutoSync.v1");
}

QString TaskSchedulerBackend::buildActionArguments(const QString &configPath, const ScheduledTaskKind kind)
{
    return (kind == ScheduledTaskKind::Startup
                ? QStringLiteral("--sync-once --scheduled --startup --config %1")
                : QStringLiteral("--sync-once --scheduled --config %1"))
        .arg(ElevationBroker::quoteWindowsArgument(QFileInfo(configPath).absoluteFilePath()));
}

int TaskSchedulerBackend::parseRepetitionIntervalMinutes(const QString &iso8601)
{
    const QString text = iso8601.trimmed().toUpper();
    static const QRegularExpression pattern(
        QStringLiteral("^P(?:([0-9]+)D)?(?:T(?:([0-9]+)H)?(?:([0-9]+)M)?(?:([0-9]+)S)?)?$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch() || text == QStringLiteral("P") || text == QStringLiteral("PT")) {
        return 0;
    }

    const auto captured = [](const QRegularExpressionMatch &current, const int index) {
        if (!current.capturedView(index).isEmpty()) {
            bool converted = false;
            const int value = current.captured(index).toInt(&converted);
            if (converted && value >= 0) {
                return value;
            }
        }
        return 0;
    };

    const qint64 days = captured(match, 1);
    const qint64 hours = captured(match, 2);
    const qint64 minutes = captured(match, 3);
    const qint64 seconds = captured(match, 4);
    const qint64 totalMinutes = days * 24 * 60 + hours * 60 + minutes + (seconds + 59) / 60;
    if (totalMinutes <= 0 || totalMinutes > 10080) {
        return 0;
    }
    return static_cast<int>(totalMinutes);
}

Result<ScheduledTaskInfo> TaskSchedulerBackend::query() const
{
#ifndef Q_OS_WIN
    return Result<ScheduledTaskInfo>::failure(
        schedulerError(ErrorCode::UnsupportedPlatform, QStringLiteral("Task Scheduler is only available on Windows")));
#else
    ComApartmentGuard apartment;
    bool uninitialize = false;
    const Result<ComPtr<ITaskService>> connected = connectService(&uninitialize);
    if (!connected) {
        return Result<ScheduledTaskInfo>::failure(connected.error());
    }
    apartment.setActive(uninitialize);
    const Result<ComPtr<ITaskFolder>> root = rootFolder(connected.value().get());
    if (!root) {
        return Result<ScheduledTaskInfo>::failure(root.error());
    }
    const Result<ScheduledTaskInfo> result = queryTask(root.value().get(), kind_);
    return result;
#endif
}

Result<ScheduledTaskInfo> TaskSchedulerBackend::install(const QString &executablePath,
                                                        const QString &configPath,
                                                        const int intervalMinutes) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(executablePath)
    Q_UNUSED(configPath)
    Q_UNUSED(intervalMinutes)
    return Result<ScheduledTaskInfo>::failure(
        schedulerError(ErrorCode::UnsupportedPlatform, QStringLiteral("Task Scheduler is only available on Windows")));
#else
    const Result<void> elevated = ensureElevated();
    if (!elevated) {
        return Result<ScheduledTaskInfo>::failure(elevated.error());
    }
    const QFileInfo executable(QFileInfo(executablePath).absoluteFilePath());
    const QFileInfo config(QFileInfo(configPath).absoluteFilePath());
    if (!executable.isAbsolute() || !executable.isFile() || !config.isAbsolute() || !config.isFile()
        || (kind_ == ScheduledTaskKind::Periodic && (intervalMinutes < 1 || intervalMinutes > 10080))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::InvalidArgument, QStringLiteral("invalid task action or interval")));
    }
    const Result<QString> userSid = currentUserSid();
    if (!userSid) {
        return Result<ScheduledTaskInfo>::failure(userSid.error());
    }

    ComApartmentGuard apartment;
    bool uninitialize = false;
    const Result<ComPtr<ITaskService>> connected = connectService(&uninitialize);
    if (!connected) {
        return Result<ScheduledTaskInfo>::failure(connected.error());
    }
    apartment.setActive(uninitialize);
    const Result<ComPtr<ITaskFolder>> rootResult = rootFolder(connected.value().get());
    if (!rootResult) {
        return Result<ScheduledTaskInfo>::failure(rootResult.error());
    }
    ITaskFolder *root = rootResult.value().get();
    const Result<ScheduledTaskInfo> existing = queryTask(root, kind_, false);
    if (!existing) {
        return existing;
    }
    ComPtr<ITaskDefinition> definition;
    HRESULT result = connected.value()->NewTask(0, definition.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot create task definition"), result));
    }

    ComPtr<IRegistrationInfo> registration;
    ScopedBstr author(L"TimeSync");
    result = definition->get_RegistrationInfo(registration.out());
    if (FAILED(result) || FAILED(registration->put_Author(author.get()))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task ownership"), result));
    }
    ComPtr<IPrincipal> principal;
    ScopedBstr user(userSid.value().toStdWString());
    result = definition->get_Principal(principal.out());
    if (FAILED(result) || FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN))
        || FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))
        || FAILED(principal->put_UserId(user.get()))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task principal"), result));
    }

    ComPtr<ITaskSettings> settings;
    result = definition->get_Settings(settings.out());
    if (FAILED(result) || FAILED(settings->put_Enabled(VARIANT_TRUE))
        || FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE))
        || FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW))
        || (kind_ == ScheduledTaskKind::Startup
            && (FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE))
                || FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE))
                || FAILED(settings->put_RunOnlyIfNetworkAvailable(VARIANT_FALSE))
                || FAILED(settings->put_RestartCount(0))))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task settings"), result));
    }
    ScopedBstr executionLimit(L"PT5M");
    result = settings->put_ExecutionTimeLimit(executionLimit.get());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task limit"), result));
    }

    ComPtr<ITriggerCollection> triggers;
    result = definition->get_Triggers(triggers.out());
    ComPtr<ITrigger> genericTrigger;
    if (SUCCEEDED(result)) {
        result = triggers->Create(kind_ == ScheduledTaskKind::Startup ? TASK_TRIGGER_LOGON : TASK_TRIGGER_TIME,
                                  genericTrigger.out());
    }
    if (SUCCEEDED(result)) {
        result = genericTrigger->put_Enabled(VARIANT_TRUE);
    }
    if (kind_ == ScheduledTaskKind::Startup) {
        ComPtr<SchedulerLogonTrigger> trigger;
        if (SUCCEEDED(result)) {
            result = genericTrigger->QueryInterface(LogonTriggerIid,
                                                    reinterpret_cast<void **>(trigger.out()));
        }
        if (SUCCEEDED(result)) {
            result = trigger->put_UserId(user.get());
        }
        if (SUCCEEDED(result)) {
            ScopedBstr delay(L"PT30S");
            result = trigger->put_Delay(delay.get());
        }
        // No repetition pattern or scheduler restart policy: retries stay inside one invocation.
    } else {
        ComPtr<ITimeTrigger> trigger;
        if (SUCCEEDED(result)) {
            result = genericTrigger->QueryInterface(IID_ITimeTrigger,
                                                    reinterpret_cast<void **>(trigger.out()));
        }
        if (SUCCEEDED(result)) {
            const QString boundary = QDateTime::currentDateTime().toString(Qt::ISODate);
            ScopedBstr startBoundary(boundary.toStdWString());
            result = trigger->put_StartBoundary(startBoundary.get());
            ComPtr<IRepetitionPattern> repetition;
            if (SUCCEEDED(result)) {
                result = trigger->get_Repetition(repetition.out());
            }
            if (SUCCEEDED(result)) {
                const QString interval = QStringLiteral("PT%1M").arg(intervalMinutes);
                ScopedBstr intervalValue(interval.toStdWString());
                result = repetition->put_Interval(intervalValue.get());
                if (SUCCEEDED(result)) {
                    ScopedBstr duration(L"P9999D");
                    result = repetition->put_Duration(duration.get());
                }
                if (SUCCEEDED(result)) {
                    result = repetition->put_StopAtDurationEnd(VARIANT_FALSE);
                }
            }
        }
    }
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task trigger"), result));
    }

    ComPtr<IActionCollection> actions;
    result = definition->get_Actions(actions.out());
    ComPtr<IAction> genericAction;
    if (SUCCEEDED(result)) {
        result = actions->Create(TASK_ACTION_EXEC, genericAction.out());
    }
    ComPtr<IExecAction> action;
    if (SUCCEEDED(result)) {
        result = genericAction->QueryInterface(IID_IExecAction,
                                               reinterpret_cast<void **>(action.out()));
    }
    if (SUCCEEDED(result)) {
        ScopedBstr path(executable.absoluteFilePath().toStdWString());
        result = action->put_Path(path.get());
        if (SUCCEEDED(result)) {
            const QString arguments = buildActionArguments(config.absoluteFilePath(), kind_);
            ScopedBstr argumentValue(arguments.toStdWString());
            result = action->put_Arguments(argumentValue.get());
        }
    }
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task action"), result));
    }

    ScopedBstr name(taskName(kind_).toStdWString());
    ScopedVariant empty;
    ScopedVariant registeredUser;
    registeredUser.get()->vt = VT_BSTR;
    registeredUser.get()->bstrVal = SysAllocString(user.get());
    ComPtr<IRegisteredTask> registeredTask;
    result = root->RegisterTaskDefinition(name.get(),
                                          definition.get(),
                                          existing.value().exists ? TASK_UPDATE : TASK_CREATE,
                                          *registeredUser.get(),
                                          *empty.get(),
                                          TASK_LOGON_INTERACTIVE_TOKEN,
                                          *empty.get(),
                                          registeredTask.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(result == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                    : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("cannot register scheduled task"),
                           static_cast<quint32>(result)));
    }

    const Result<ScheduledTaskInfo> verified = queryTask(root, kind_);
    if (!verified) {
        return verified;
    }
    if (!verified.value().exists || !verified.value().enabled
        || verified.value().intervalMinutes != (kind_ == ScheduledTaskKind::Startup ? 0 : intervalMinutes)
        || !sameExecutable(verified.value().executablePath, executable.absoluteFilePath())
        || verified.value().arguments != buildActionArguments(config.absoluteFilePath(), kind_)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("scheduled task verification failed")));
    }
    return verified;
#endif
}

Result<ScheduledTaskInfo> TaskSchedulerBackend::setEnabled(const bool enabled) const
{
#ifndef Q_OS_WIN
    Q_UNUSED(enabled)
    return Result<ScheduledTaskInfo>::failure(
        schedulerError(ErrorCode::UnsupportedPlatform, QStringLiteral("Task Scheduler is only available on Windows")));
#else
    const Result<void> elevated = ensureElevated();
    if (!elevated) {
        return Result<ScheduledTaskInfo>::failure(elevated.error());
    }
    ComApartmentGuard apartment;
    bool uninitialize = false;
    const Result<ComPtr<ITaskService>> connected = connectService(&uninitialize);
    if (!connected) {
        return Result<ScheduledTaskInfo>::failure(connected.error());
    }
    apartment.setActive(uninitialize);
    const Result<ComPtr<ITaskFolder>> rootResult = rootFolder(connected.value().get());
    if (!rootResult) {
        return Result<ScheduledTaskInfo>::failure(rootResult.error());
    }
    ITaskFolder *root = rootResult.value().get();
    const Result<ScheduledTaskInfo> existing = queryTask(root, kind_, enabled);
    if (!existing) {
        return existing;
    }
    if ((!enabled && !existing.value().exists)
        || (existing.value().exists && existing.value().enabled == enabled)) {
        return existing;
    }
    ScopedBstr name(taskName(kind_).toStdWString());
    ComPtr<IRegisteredTask> task;
    const HRESULT found = root->GetTask(name.get(), task.out());
    if (FAILED(found)) {
        if (!enabled && found == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            return Result<ScheduledTaskInfo>::success({});
        }
        return Result<ScheduledTaskInfo>::failure(
            found == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                ? schedulerError(ErrorCode::TaskNotFound, QStringLiteral("scheduled task does not exist"), found)
                : schedulerError(found == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                         : ErrorCode::SchedulerMutationFailed,
                                 QStringLiteral("cannot open scheduled task"),
                                 static_cast<quint32>(found)));
    }
    const HRESULT changed = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
    if (FAILED(changed)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(changed == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                     : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("cannot change scheduled task state"),
                           static_cast<quint32>(changed)));
    }
    const Result<ScheduledTaskInfo> verified = queryTask(root, kind_, enabled);
    if (!verified) {
        return verified;
    }
    if (!verified.value().exists || verified.value().enabled != enabled) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("scheduled task state verification failed")));
    }
    return verified;
#endif
}

Result<void> TaskSchedulerBackend::remove() const
{
#ifndef Q_OS_WIN
    return Result<void>::failure(
        schedulerError(ErrorCode::UnsupportedPlatform, QStringLiteral("Task Scheduler is only available on Windows")));
#else
    const Result<void> elevated = ensureElevated();
    if (!elevated) {
        return elevated;
    }
    ComApartmentGuard apartment;
    bool uninitialize = false;
    const Result<ComPtr<ITaskService>> connected = connectService(&uninitialize);
    if (!connected) {
        return Result<void>::failure(connected.error());
    }
    apartment.setActive(uninitialize);
    const Result<ComPtr<ITaskFolder>> rootResult = rootFolder(connected.value().get());
    if (!rootResult) {
        return Result<void>::failure(rootResult.error());
    }
    ITaskFolder *root = rootResult.value().get();
    const Result<ScheduledTaskInfo> existing = queryTask(root, kind_, false);
    if (!existing) {
        return Result<void>::failure(existing.error());
    }
    if (!existing.value().exists) {
        return Result<void>::success();
    }
    ScopedBstr name(taskName(kind_).toStdWString());
    const HRESULT deleted = root->DeleteTask(name.get(), 0);
    if (FAILED(deleted) && deleted != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return Result<void>::failure(
            schedulerError(deleted == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                     : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("cannot remove scheduled task"),
                           static_cast<quint32>(deleted)));
    }
    const Result<ScheduledTaskInfo> verified = queryTask(root, kind_);
    if (!verified) {
        return Result<void>::failure(verified.error());
    }
    if (verified.value().exists) {
        return Result<void>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("scheduled task still exists")));
    }
    return Result<void>::success();
#endif
}

} // namespace TimeSync
