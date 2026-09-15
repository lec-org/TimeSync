#include "about_dialog.h"

#include "brand_assets.h"
#include "ui_strings.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

namespace TimeSync::Ui {
namespace {

constexpr int LogoLogicalHeight = 132;

QString linkHtml(const QString &url, const QString &display)
{
    return QStringLiteral(
               "<a href=\"%1\" style=\"color:#005AAE; text-decoration:none; font-weight:600;\">%2</a>")
        .arg(url.toHtmlEscaped(), display.toHtmlEscaped());
}

void configureLinkLabel(QLabel *label)
{
    label->setObjectName(QStringLiteral("aboutLink"));
    label->setTextFormat(Qt::RichText);
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setFocusPolicy(Qt::TabFocus);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setWordWrap(true);
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("aboutDialog"));
    setModal(true);
    setWindowFlag(Qt::MSWindowsFixedSizeDialogHint, true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumWidth(420);
    buildUi();
    retranslateUi();
}

void AboutDialog::showEvent(QShowEvent *event)
{
    applyLogo();
    QDialog::showEvent(event);
}

void AboutDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(0);

    logoLabel_ = new QLabel(this);
    logoLabel_->setObjectName(QStringLiteral("aboutLogo"));
    logoLabel_->setAlignment(Qt::AlignCenter);
    applyLogo();

    studioLabel_ = new QLabel(this);
    studioLabel_->setObjectName(QStringLiteral("aboutStudio"));
    studioLabel_->setAlignment(Qt::AlignCenter);
    studioLabel_->setWordWrap(true);

    auto *linksFrame = new QFrame(this);
    linksFrame->setObjectName(QStringLiteral("aboutLinksFrame"));
    auto *linksLayout = new QGridLayout(linksFrame);
    linksLayout->setContentsMargins(16, 14, 16, 14);
    linksLayout->setHorizontalSpacing(16);
    linksLayout->setVerticalSpacing(10);

    websiteCaptionLabel_ = new QLabel(linksFrame);
    websiteCaptionLabel_->setObjectName(QStringLiteral("aboutCaption"));
    websiteLinkLabel_ = new QLabel(linksFrame);
    configureLinkLabel(websiteLinkLabel_);

    githubCaptionLabel_ = new QLabel(linksFrame);
    githubCaptionLabel_->setObjectName(QStringLiteral("aboutCaption"));
    githubLinkLabel_ = new QLabel(linksFrame);
    configureLinkLabel(githubLinkLabel_);

    linksLayout->addWidget(websiteCaptionLabel_, 0, 0);
    linksLayout->addWidget(websiteLinkLabel_, 0, 1);
    linksLayout->addWidget(githubCaptionLabel_, 1, 0);
    linksLayout->addWidget(githubLinkLabel_, 1, 1);
    linksLayout->setColumnStretch(1, 1);

    closeButton_ = new QPushButton(this);
    closeButton_->setObjectName(QStringLiteral("primaryButton"));
    closeButton_->setCursor(Qt::PointingHandCursor);
    closeButton_->setDefault(true);
    closeButton_->setMinimumWidth(120);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);

    root->addWidget(logoLabel_, 0, Qt::AlignHCenter);
    root->addSpacing(12);
    root->addWidget(studioLabel_);
    root->addSpacing(18);
    root->addWidget(linksFrame);
    root->addSpacing(20);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(closeButton_);
    buttonRow->addStretch(1);
    root->addLayout(buttonRow);
}

void AboutDialog::applyLogo()
{
    const QPixmap logo = brandLogoPixmap(LogoLogicalHeight);
    logoLabel_->setPixmap(logo);
    if (!logo.isNull()) {
        logoLabel_->setFixedSize(logo.deviceIndependentSize().toSize());
    }
}

void AboutDialog::retranslateUi()
{
    setWindowTitle(UiStrings::text(QStringLiteral("about.title")));
    studioLabel_->setText(UiStrings::text(QStringLiteral("about.studio")));
    logoLabel_->setAccessibleName(UiStrings::text(QStringLiteral("about.logoAlt")));
    websiteCaptionLabel_->setText(UiStrings::text(QStringLiteral("about.website")));
    githubCaptionLabel_->setText(UiStrings::text(QStringLiteral("about.github")));
    websiteLinkLabel_->setText(linkHtml(QStringLiteral("https://lec-page-2026.ziroo.cn/"),
                                        QStringLiteral("lec-page-2026.ziroo.cn")));
    githubLinkLabel_->setText(linkHtml(QStringLiteral("https://github.com/lec-org"),
                                       QStringLiteral("github.com/lec-org")));
    closeButton_->setText(UiStrings::text(QStringLiteral("action.close")));
}

} // namespace TimeSync::Ui
