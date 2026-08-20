#include "server_editor_dialog.h"

#include "ui_strings.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSize>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace TimeSync::Ui {
namespace {

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

bool isValidPort(const QString &value)
{
    bool converted = false;
    const int port = value.toInt(&converted);
    return converted && port >= 1 && port <= 65535;
}

bool isValidIpv4(const QString &value)
{
    const QStringList parts = value.split(QLatin1Char('.'));
    if (parts.size() != 4) {
        return false;
    }

    for (const QString &part : parts) {
        if (part.isEmpty() || part.size() > 3) {
            return false;
        }
        bool converted = false;
        const int octet = part.toInt(&converted);
        if (!converted || octet < 0 || octet > 255) {
            return false;
        }
    }
    return true;
}

bool isValidIpv6(const QString &value)
{
    if (value.isEmpty() || value.contains(QLatin1Char('%'))
        || value.indexOf(QStringLiteral("::")) != value.lastIndexOf(QStringLiteral("::"))) {
        return false;
    }

    const bool compressed = value.contains(QStringLiteral("::"));
    const QStringList groups = value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    int groupCount = 0;
    static const QRegularExpression hexGroup(QStringLiteral("^[0-9A-Fa-f]{1,4}$"));

    for (int index = 0; index < groups.size(); ++index) {
        const QString &group = groups.at(index);
        if (group.contains(QLatin1Char('.'))) {
            if (index != groups.size() - 1 || !isValidIpv4(group)) {
                return false;
            }
            groupCount += 2;
            continue;
        }
        if (!hexGroup.match(group).hasMatch()) {
            return false;
        }
        ++groupCount;
    }

    return compressed ? groupCount < 8 : groupCount == 8;
}

QString normalizedServerKey(const QString &address)
{
    QString value = address.toCaseFolded();
    if (value.startsWith(QLatin1Char('['))) {
        const qsizetype closingBracket = value.indexOf(QLatin1Char(']'));
        if (closingBracket > 0 && value.mid(closingBracket + 1) == QStringLiteral(":123")) {
            value.truncate(closingBracket + 1);
        }
        return value;
    }

    if (value.count(QLatin1Char(':')) <= 1) {
        const qsizetype separator = value.lastIndexOf(QLatin1Char(':'));
        QString host = separator < 0 ? value : value.left(separator);
        const QString port = separator < 0 ? QString() : value.mid(separator + 1);
        if (host.endsWith(QLatin1Char('.'))) {
            host.chop(1);
        }
        return port.isEmpty() || port == QStringLiteral("123")
            ? host
            : host + QLatin1Char(':') + port;
    }
    return value;
}

} // namespace

ServerEditorDialog::ServerEditorDialog(const QStringList &servers,
                                       const QStringList &defaultServers,
                                       QWidget *parent)
    : QDialog(parent)
    , defaultServers_(defaultServers)
{
    buildUi();
    serverList_->addItems(servers);
    if (serverList_->count() > 0) {
        serverList_->setCurrentRow(0);
    }
    retranslateUi();
    updateControlState();
}

QStringList ServerEditorDialog::servers() const
{
    QStringList values;
    values.reserve(serverList_->count());
    for (int row = 0; row < serverList_->count(); ++row) {
        values.append(serverList_->item(row)->text());
    }
    return values;
}

void ServerEditorDialog::retranslateUi()
{
    setWindowTitle(UiStrings::text(QStringLiteral("servers.dialog.title")));
    addressLabel_->setText(UiStrings::text(QStringLiteral("servers.dialog.addressLabel")));
    addressEdit_->setPlaceholderText(UiStrings::text(QStringLiteral("servers.dialog.placeholder")));

    addButton_->setToolTip(UiStrings::text(QStringLiteral("servers.dialog.add")));
    addButton_->setAccessibleName(UiStrings::text(QStringLiteral("servers.dialog.add")));
    addButton_->setText(UiStrings::text(QStringLiteral("servers.dialog.add")));
    deleteButton_->setToolTip(UiStrings::text(QStringLiteral("servers.dialog.delete")));
    deleteButton_->setAccessibleName(UiStrings::text(QStringLiteral("servers.dialog.delete")));
    moveUpButton_->setToolTip(UiStrings::text(QStringLiteral("servers.dialog.moveUp")));
    moveUpButton_->setAccessibleName(UiStrings::text(QStringLiteral("servers.dialog.moveUp")));
    moveDownButton_->setToolTip(UiStrings::text(QStringLiteral("servers.dialog.moveDown")));
    moveDownButton_->setAccessibleName(UiStrings::text(QStringLiteral("servers.dialog.moveDown")));

    restoreButton_->setText(UiStrings::text(QStringLiteral("action.restoreDefaults")));
    cancelButton_->setText(UiStrings::text(QStringLiteral("action.cancel")));
    saveButton_->setText(saving_ ? UiStrings::text(QStringLiteral("busy.saving"))
                                 : UiStrings::text(QStringLiteral("action.save")));
    serverList_->setAccessibleName(UiStrings::text(QStringLiteral("servers.heading")));
    if (!validationMessageKey_.isEmpty()) {
        validationLabel_->setText(UiStrings::text(validationMessageKey_));
    }
}

void ServerEditorDialog::setSaving(bool saving)
{
    saving_ = saving;
    retranslateUi();
    updateControlState();
}

void ServerEditorDialog::showSaveError(const QString &message)
{
    saving_ = false;
    validationMessageKey_.clear();
    validationLabel_->setText(message);
    validationLabel_->setProperty("visibleError", true);
    repolish(validationLabel_);
    retranslateUi();
    updateControlState();
    saveButton_->setFocus(Qt::OtherFocusReason);
}

void ServerEditorDialog::buildUi()
{
    setModal(true);
    setMinimumSize(560, 460);
    resize(620, 500);
    setObjectName(QStringLiteral("serverEditorDialog"));

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(24, 24, 24, 20);
    rootLayout->setSpacing(12);

    addressLabel_ = new QLabel(this);
    addressLabel_->setObjectName(QStringLiteral("fieldLabel"));
    rootLayout->addWidget(addressLabel_);

    auto *addressLayout = new QHBoxLayout;
    addressLayout->setSpacing(8);
    addressEdit_ = new QLineEdit(this);
    addressEdit_->setClearButtonEnabled(true);
    addressEdit_->setMinimumHeight(40);
    addressLabel_->setBuddy(addressEdit_);
    addButton_ = new QPushButton(this);
    addButton_->setObjectName(QStringLiteral("secondaryButton"));
    addButton_->setMinimumSize(92, 40);
    addressLayout->addWidget(addressEdit_, 1);
    addressLayout->addWidget(addButton_);
    rootLayout->addLayout(addressLayout);

    auto *listLayout = new QHBoxLayout;
    listLayout->setSpacing(8);
    serverList_ = new QListWidget(this);
    serverList_->setSelectionMode(QAbstractItemView::SingleSelection);
    serverList_->setAlternatingRowColors(false);
    serverList_->setMinimumHeight(230);
    listLayout->addWidget(serverList_, 1);

    auto *toolLayout = new QVBoxLayout;
    toolLayout->setSpacing(8);
    deleteButton_ = new QToolButton(this);
    deleteButton_->setObjectName(QStringLiteral("iconButton"));
    deleteButton_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    deleteButton_->setIconSize(QSize(18, 18));
    deleteButton_->setMinimumSize(40, 40);
    moveUpButton_ = new QToolButton(this);
    moveUpButton_->setObjectName(QStringLiteral("iconButton"));
    moveUpButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    moveUpButton_->setIconSize(QSize(18, 18));
    moveUpButton_->setMinimumSize(40, 40);
    moveDownButton_ = new QToolButton(this);
    moveDownButton_->setObjectName(QStringLiteral("iconButton"));
    moveDownButton_->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    moveDownButton_->setIconSize(QSize(18, 18));
    moveDownButton_->setMinimumSize(40, 40);
    toolLayout->addWidget(deleteButton_);
    toolLayout->addWidget(moveUpButton_);
    toolLayout->addWidget(moveDownButton_);
    toolLayout->addStretch(1);
    listLayout->addLayout(toolLayout);
    rootLayout->addLayout(listLayout, 1);

    validationLabel_ = new QLabel(this);
    validationLabel_->setObjectName(QStringLiteral("validationLabel"));
    validationLabel_->setWordWrap(true);
    validationLabel_->setMinimumHeight(36);
    rootLayout->addWidget(validationLabel_);

    auto *divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setObjectName(QStringLiteral("divider"));
    rootLayout->addWidget(divider);

    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(8);
    restoreButton_ = new QPushButton(this);
    restoreButton_->setObjectName(QStringLiteral("secondaryButton"));
    cancelButton_ = new QPushButton(this);
    cancelButton_->setObjectName(QStringLiteral("secondaryButton"));
    saveButton_ = new QPushButton(this);
    saveButton_->setObjectName(QStringLiteral("primaryButton"));
    saveButton_->setDefault(true);
    buttonLayout->addWidget(restoreButton_);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(cancelButton_);
    buttonLayout->addWidget(saveButton_);
    rootLayout->addLayout(buttonLayout);

    connect(addressEdit_, &QLineEdit::returnPressed, this, &ServerEditorDialog::addServer);
    connect(addressEdit_, &QLineEdit::textChanged, this, [this] { clearValidation(); });
    connect(addButton_, &QPushButton::clicked, this, &ServerEditorDialog::addServer);
    connect(deleteButton_, &QToolButton::clicked, this, &ServerEditorDialog::deleteSelectedServer);
    connect(moveUpButton_, &QToolButton::clicked, this, [this] { moveSelectedServer(-1); });
    connect(moveDownButton_, &QToolButton::clicked, this, [this] { moveSelectedServer(1); });
    connect(serverList_, &QListWidget::currentRowChanged, this, [this] { updateControlState(); });
    connect(restoreButton_, &QPushButton::clicked, this, &ServerEditorDialog::restoreDefaults);
    connect(cancelButton_, &QPushButton::clicked, this, &ServerEditorDialog::reject);
    connect(saveButton_, &QPushButton::clicked, this, &ServerEditorDialog::requestSave);

    QWidget::setTabOrder(addressEdit_, addButton_);
    QWidget::setTabOrder(addButton_, serverList_);
    QWidget::setTabOrder(serverList_, deleteButton_);
    QWidget::setTabOrder(deleteButton_, moveUpButton_);
    QWidget::setTabOrder(moveUpButton_, moveDownButton_);
    QWidget::setTabOrder(moveDownButton_, restoreButton_);
    QWidget::setTabOrder(restoreButton_, cancelButton_);
    QWidget::setTabOrder(cancelButton_, saveButton_);
}

void ServerEditorDialog::addServer()
{
    clearValidation();
    const QString address = addressEdit_->text().trimmed();
    if (address.isEmpty()) {
        showValidation({false, -1, QStringLiteral("servers.validation.emptyAddress")});
        addressEdit_->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (address != addressEdit_->text() || !isValidServerAddress(address)) {
        showValidation({false, -1, QStringLiteral("servers.validation.invalidAddress")});
        addressEdit_->selectAll();
        addressEdit_->setFocus(Qt::OtherFocusReason);
        return;
    }

    for (int row = 0; row < serverList_->count(); ++row) {
        if (normalizedServerKey(serverList_->item(row)->text()) == normalizedServerKey(address)) {
            showValidation({false, row, QStringLiteral("servers.validation.duplicate")});
            serverList_->setCurrentRow(row);
            return;
        }
    }

    serverList_->addItem(address);
    serverList_->setCurrentRow(serverList_->count() - 1);
    addressEdit_->clear();
    addressEdit_->setFocus(Qt::OtherFocusReason);
    updateControlState();
}

void ServerEditorDialog::deleteSelectedServer()
{
    const int row = serverList_->currentRow();
    if (row < 0 || saving_) {
        return;
    }
    delete serverList_->takeItem(row);
    if (serverList_->count() > 0) {
        serverList_->setCurrentRow(qMin(row, serverList_->count() - 1));
    }
    clearValidation();
    updateControlState();
}

void ServerEditorDialog::moveSelectedServer(int offset)
{
    const int row = serverList_->currentRow();
    const int targetRow = row + offset;
    if (row < 0 || targetRow < 0 || targetRow >= serverList_->count() || saving_) {
        return;
    }
    QListWidgetItem *item = serverList_->takeItem(row);
    serverList_->insertItem(targetRow, item);
    serverList_->setCurrentRow(targetRow);
    clearValidation();
}

void ServerEditorDialog::restoreDefaults()
{
    if (saving_) {
        return;
    }
    serverList_->clear();
    serverList_->addItems(defaultServers_);
    if (serverList_->count() > 0) {
        serverList_->setCurrentRow(0);
    }
    clearValidation();
    updateControlState();
}

void ServerEditorDialog::requestSave()
{
    clearValidation();
    const ValidationResult validation = validateServers();
    if (!validation.valid) {
        showValidation(validation);
        return;
    }

    setSaving(true);
    emit saveRequested(servers());
}

void ServerEditorDialog::updateControlState()
{
    const int row = serverList_->currentRow();
    const bool hasSelection = row >= 0;
    addressEdit_->setEnabled(!saving_);
    addButton_->setEnabled(!saving_);
    serverList_->setEnabled(!saving_);
    deleteButton_->setEnabled(!saving_ && hasSelection);
    moveUpButton_->setEnabled(!saving_ && hasSelection && row > 0);
    moveDownButton_->setEnabled(!saving_ && hasSelection && row < serverList_->count() - 1);
    restoreButton_->setEnabled(!saving_ && !defaultServers_.isEmpty());
    cancelButton_->setEnabled(!saving_);
    saveButton_->setEnabled(!saving_);
}

void ServerEditorDialog::clearValidation()
{
    validationMessageKey_.clear();
    validationLabel_->clear();
    validationLabel_->setProperty("visibleError", false);
    addressEdit_->setProperty("error", false);
    repolish(validationLabel_);
    repolish(addressEdit_);
    for (int row = 0; row < serverList_->count(); ++row) {
        serverList_->item(row)->setForeground(QBrush());
    }
}

void ServerEditorDialog::showValidation(const ValidationResult &result)
{
    validationMessageKey_ = result.messageKey;
    validationLabel_->setText(UiStrings::text(result.messageKey));
    validationLabel_->setProperty("visibleError", true);
    repolish(validationLabel_);

    if (result.row >= 0 && result.row < serverList_->count()) {
        serverList_->setCurrentRow(result.row);
        serverList_->item(result.row)->setForeground(QColor(QStringLiteral("#B42318")));
        serverList_->scrollToItem(serverList_->item(result.row));
        serverList_->setFocus(Qt::OtherFocusReason);
    } else {
        addressEdit_->setProperty("error", true);
        repolish(addressEdit_);
    }
}

ServerEditorDialog::ValidationResult ServerEditorDialog::validateServers() const
{
    if (serverList_->count() == 0) {
        return {false, -1, QStringLiteral("servers.validation.atLeastOne")};
    }

    QSet<QString> normalizedAddresses;
    for (int row = 0; row < serverList_->count(); ++row) {
        const QString address = serverList_->item(row)->text();
        if (address != address.trimmed()
            || address.contains(QRegularExpression(QStringLiteral("\\s")))) {
            return {false, row, QStringLiteral("servers.validation.invalidCharacters")};
        }
        if (!isValidServerAddress(address)) {
            return {false, row, QStringLiteral("servers.validation.invalidAddress")};
        }

        const QString normalized = normalizedServerKey(address);
        if (normalizedAddresses.contains(normalized)) {
            return {false, row, QStringLiteral("servers.validation.duplicate")};
        }
        normalizedAddresses.insert(normalized);
    }
    return {};
}

bool ServerEditorDialog::isValidServerAddress(const QString &address)
{
    if (address.isEmpty() || address.size() > 260
        || address.contains(QRegularExpression(QStringLiteral("\\s")))) {
        return false;
    }

    QString host = address;
    if (address.startsWith(QLatin1Char('['))) {
        const qsizetype closingBracket = address.indexOf(QLatin1Char(']'));
        if (closingBracket <= 1) {
            return false;
        }
        host = address.mid(1, closingBracket - 1);
        const QString remainder = address.mid(closingBracket + 1);
        if (!remainder.isEmpty()
            && (!remainder.startsWith(QLatin1Char(':')) || !isValidPort(remainder.mid(1)))) {
            return false;
        }
        return isValidIpv6(host);
    }

    const int colonCount = address.count(QLatin1Char(':'));
    if (colonCount > 1) {
        return isValidIpv6(address);
    }
    if (colonCount == 1) {
        const qsizetype separator = address.lastIndexOf(QLatin1Char(':'));
        host = address.left(separator);
        if (host.isEmpty() || !isValidPort(address.mid(separator + 1))) {
            return false;
        }
    }

    if (isValidIpv4(host)) {
        return true;
    }

    static const QRegularExpression hostName(
        QStringLiteral("^(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)"
                       "(?:\\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*\\.?$"));
    return hostName.match(host).hasMatch();
}

} // namespace TimeSync::Ui
