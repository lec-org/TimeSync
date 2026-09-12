#include "task_scheduler.h"

#include "elevation_broker.h"

#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <taskschd.h>
#    include <oleauto.h>
#endif

namespace TimeSync {
namespace {

Error schedulerError(const ErrorCode code, const QString &detail, const quint32 nativeCode = 0)
{
    return {code, detail, nativeCode};
}

#ifdef Q_OS_WIN
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

Result<ScheduledTaskInfo> queryTask(ITaskFolder *root)
{
    ScopedBstr name(TaskSchedulerBackend::taskName().toStdWString());
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
    ComPtr<IActionCollection> actions;
    result = definition->get_Actions(actions.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot query task action"), result));
    }
    LONG actionCount = 0;
    actions->get_Count(&actionCount);
    if (actionCount < 1) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("task has no action")));
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
    if (SUCCEEDED(exec->get_Arguments(&arguments))) {
        info.arguments = bstrToQString(arguments);
    }

    ComPtr<ITriggerCollection> triggers;
    result = definition->get_Triggers(triggers.out());
    if (SUCCEEDED(result)) {
        LONG triggerCount = 0;
        triggers->get_Count(&triggerCount);
        if (triggerCount > 0) {
            ComPtr<ITrigger> trigger;
            if (SUCCEEDED(triggers->get_Item(1, trigger.out()))) {
                ComPtr<ITimeTrigger> timeTrigger;
                if (SUCCEEDED(trigger->QueryInterface(IID_ITimeTrigger,
                                                       reinterpret_cast<void **>(timeTrigger.out())))) {
                    ComPtr<IRepetitionPattern> repetition;
                    if (SUCCEEDED(timeTrigger->get_Repetition(repetition.out()))) {
                        BSTR interval = nullptr;
                        if (SUCCEEDED(repetition->get_Interval(&interval))) {
                            const QString intervalText = bstrToQString(interval);
                            info.intervalMinutes = TaskSchedulerBackend::parseRepetitionIntervalMinutes(
                                intervalText);
                        }
                    }
                }
            }
        }
    }
    return Result<ScheduledTaskInfo>::success(std::move(info));
}

#endif

} // namespace

QString TaskSchedulerBackend::taskName()
{
    return QStringLiteral("TimeSync.StandardClock.AutoSync.v1");
}

QString TaskSchedulerBackend::buildActionArguments(const QString &configPath)
{
    return QStringLiteral("--sync-once --scheduled --config %1")
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
    const Result<ScheduledTaskInfo> result = queryTask(root.value().get());
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
        || intervalMinutes < 1 || intervalMinutes > 10080) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::InvalidArgument, QStringLiteral("invalid task action or interval")));
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
    ComPtr<ITaskDefinition> definition;
    HRESULT result = connected.value()->NewTask(0, definition.out());
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot create task definition"), result));
    }

    ComPtr<IRegistrationInfo> registration;
    if (SUCCEEDED(definition->get_RegistrationInfo(registration.out()))) {
        ScopedBstr author(L"TimeSync");
        registration->put_Author(author.get());
    }
    ComPtr<IPrincipal> principal;
    result = definition->get_Principal(principal.out());
    if (FAILED(result) || FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN))
        || FAILED(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task principal"), result));
    }

    ComPtr<ITaskSettings> settings;
    result = definition->get_Settings(settings.out());
    if (FAILED(result) || FAILED(settings->put_Enabled(VARIANT_TRUE))
        || FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE))
        || FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW))) {
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
        result = triggers->Create(TASK_TRIGGER_TIME, genericTrigger.out());
    }
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
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure recurring trigger"), result));
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
            const QString arguments = buildActionArguments(config.absoluteFilePath());
            ScopedBstr argumentValue(arguments.toStdWString());
            result = action->put_Arguments(argumentValue.get());
        }
    }
    if (FAILED(result)) {
        return Result<ScheduledTaskInfo>::failure(
            schedulerError(ErrorCode::SchedulerMutationFailed, QStringLiteral("cannot configure task action"), result));
    }

    ScopedBstr name(taskName().toStdWString());
    ScopedVariant empty;
    ComPtr<IRegisteredTask> registeredTask;
    result = root->RegisterTaskDefinition(name.get(),
                                          definition.get(),
                                          TASK_CREATE_OR_UPDATE,
                                          *empty.get(),
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

    const Result<ScheduledTaskInfo> verified = queryTask(root);
    if (!verified) {
        return verified;
    }
    if (!verified.value().exists || !verified.value().enabled
        || verified.value().intervalMinutes != intervalMinutes
        || QFileInfo(verified.value().executablePath).absoluteFilePath()
               != QFileInfo(executable.absoluteFilePath()).absoluteFilePath()
        || verified.value().arguments != buildActionArguments(config.absoluteFilePath())) {
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
    ScopedBstr name(taskName().toStdWString());
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
    const Result<ScheduledTaskInfo> verified = queryTask(root);
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
    ScopedBstr name(taskName().toStdWString());
    const HRESULT deleted = root->DeleteTask(name.get(), 0);
    if (FAILED(deleted) && deleted != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return Result<void>::failure(
            schedulerError(deleted == E_ACCESSDENIED ? ErrorCode::SchedulerAccessDenied
                                                     : ErrorCode::SchedulerMutationFailed,
                           QStringLiteral("cannot remove scheduled task"),
                           static_cast<quint32>(deleted)));
    }
    const Result<ScheduledTaskInfo> verified = queryTask(root);
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
