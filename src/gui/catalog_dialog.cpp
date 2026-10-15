// catalog_dialog.cpp — Blockauswahl (siehe catalog_dialog.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "catalog_dialog.hpp"

#include "theme.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace {
constexpr int kAllCategories = -1;
}

CatalogDialog::CatalogDialog(const qc::catalog::Catalog& catalog, int row, int column, QWidget* parent)
    : QDialog(parent), catalog_(catalog) {
    setWindowTitle(QStringLiteral("Place block — row %1, column %2").arg(row + 1).arg(column + 1));
    resize(900, 560);

    auto* root = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    search_ = new QLineEdit;
    search_->setPlaceholderText(QStringLiteral("search name, category or real-world gear…"));
    search_->setClearButtonEnabled(true);
    showAll_ = new QCheckBox(QStringLiteral("show internal / superseded"));
    count_ = new QLabel;
    count_->setStyleSheet(QStringLiteral("color: #8a8a90;"));
    top->addWidget(search_, 1);
    top->addWidget(showAll_);
    top->addWidget(count_);
    root->addLayout(top);

    auto* body = new QHBoxLayout;
    categories_ = new QListWidget;
    categories_->setMaximumWidth(220);
    models_ = new QListWidget;
    preview_ = new QLabel;
    preview_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    preview_->setWordWrap(true);
    preview_->setMinimumWidth(260);
    preview_->setMaximumWidth(320);
    preview_->setTextFormat(Qt::RichText);
    body->addWidget(categories_);
    body->addWidget(models_, 1);
    body->addWidget(preview_);
    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Place"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    root->addWidget(buttons);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { rebuildModels(); });
    connect(showAll_, &QCheckBox::toggled, this, [this](bool) {
        rebuildCategories();
        rebuildModels();
    });
    connect(categories_, &QListWidget::currentRowChanged, this, [this](int) { rebuildModels(); });
    connect(models_, &QListWidget::currentItemChanged, this, [this, buttons](QListWidgetItem* item, QListWidgetItem*) {
        selected_ = item ? std::optional<int>(item->data(Qt::UserRole).toInt()) : std::nullopt;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(item != nullptr);
        showPreview(item);
    });
    connect(models_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        selected_ = item->data(Qt::UserRole).toInt();
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    rebuildCategories();
    rebuildModels();
    search_->setFocus();
}

void CatalogDialog::rebuildCategories() {
    const QString keep = categories_->currentItem() ? categories_->currentItem()->text() : QString();
    categories_->blockSignals(true);
    categories_->clear();
    auto* all = new QListWidgetItem(QStringLiteral("All categories"));
    all->setData(Qt::UserRole, kAllCategories);
    categories_->addItem(all);
    int idx = 0;
    for (const auto& c : catalog_.categories()) {
        bool visible = false;
        for (const auto* m : catalog_.by_category(c)) {
            if (showAll_->isChecked() || (!m->hidden && !m->internal && !m->category_hidden && !m->superseded)) {
                visible = true;
                break;
            }
        }
        if (!visible) continue;
        auto* item = new QListWidgetItem(QString::fromStdString(c));
        item->setData(Qt::UserRole, idx++);
        item->setForeground(theme::category_color(QString::fromStdString(c)));
        categories_->addItem(item);
    }
    categories_->blockSignals(false);
    int row = 0;
    for (int i = 0; i < categories_->count(); ++i) {
        if (categories_->item(i)->text() == keep) row = i;
    }
    categories_->setCurrentRow(row);
}

void CatalogDialog::rebuildModels() {
    models_->clear();
    const QString needle = search_->text().trimmed();
    const bool all = !categories_->currentItem() || categories_->currentItem()->data(Qt::UserRole).toInt() == kAllCategories;
    const QString category = categories_->currentItem() ? categories_->currentItem()->text() : QString();
    int shown = 0;
    for (const auto& [id, m] : catalog_.models()) {
        if (!showAll_->isChecked() && (m.hidden || m.internal || m.category_hidden || m.superseded)) continue;
        if (!all && QString::fromStdString(m.category) != category) continue;
        if (!needle.isEmpty()) {
            const QString hay = QStringLiteral("%1 %2 %3").arg(QString::fromStdString(m.name), QString::fromStdString(m.category),
                                                             QString::fromStdString(m.based_on));
            if (!hay.contains(needle, Qt::CaseInsensitive)) continue;
        }
        auto* item = new QListWidgetItem(QStringLiteral("%1   %2").arg(QString::fromStdString(m.name),
                                                                     m.based_on.empty() ? QString() : QStringLiteral("(%1)").arg(QString::fromStdString(m.based_on))));
        item->setData(Qt::UserRole, id);
        item->setForeground(theme::category_color(QString::fromStdString(m.category)));
        if (m.superseded) item->setToolTip(QStringLiteral("superseded by a newer model"));
        models_->addItem(item);
        ++shown;
    }
    count_->setText(QStringLiteral("%1 of %2").arg(shown).arg(catalog_.size()));
    if (models_->count() == 1) models_->setCurrentRow(0);
}

void CatalogDialog::showPreview(QListWidgetItem* item) {
    if (!item) {
        preview_->clear();
        return;
    }
    const auto* m = catalog_.find(item->data(Qt::UserRole).toInt());
    if (!m) return;
    QString html = QStringLiteral("<b style='font-size:14px;color:%1'>%2</b><br/><span style='color:#8a8a90'>%3 · id %4%5</span>")
                       .arg(theme::category_color(QString::fromStdString(m->category)).name(), QString::fromStdString(m->name).toHtmlEscaped(),
                            QString::fromStdString(m->category).toHtmlEscaped())
                       .arg(m->id)
                       .arg(m->is_factory() ? QString() : QStringLiteral(" · not factory"));
    if (!m->based_on.empty()) html += QStringLiteral("<br/><i>%1</i>").arg(QString::fromStdString(m->based_on).toHtmlEscaped());
    html += QStringLiteral("<br/><br/><b>Parameters</b><br/>");
    int n = 0;
    for (const auto& p : m->parameters) {
        if (p.is_meter()) continue;
        html += QStringLiteral("%1 %2 <span style='color:#8a8a90'>%3</span><br/>")
                    .arg(p.index)
                    .arg(QString::fromStdString(p.name).toHtmlEscaped(),
                         p.range_is_placeholder() ? QString() : QStringLiteral("%1..%2 %3").arg(p.minimum).arg(p.maximum).arg(QString::fromStdString(p.units)));
        if (++n >= 24) {
            html += QStringLiteral("…");
            break;
        }
    }
    preview_->setText(html);
}

std::optional<int> CatalogDialog::pick(const qc::catalog::Catalog& catalog, int row, int column, QWidget* parent) {
    CatalogDialog dlg(catalog, row, column, parent);
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;
    return dlg.selectedModelId();
}
