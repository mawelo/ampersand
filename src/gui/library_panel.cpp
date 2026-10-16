// library_panel.cpp — Preset-Browser (siehe library_panel.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "library_panel.hpp"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

LibraryPanel::LibraryPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);

    auto* top = new QHBoxLayout;
    folders_ = new QComboBox;
    folders_->addItem(QStringLiteral("My Presets"), QString::fromUtf8(qc::view::kUserSetlist.data(), static_cast<qsizetype>(qc::view::kUserSetlist.size())));
    folders_->addItem(QStringLiteral("Factory Library"), QString::fromUtf8(qc::view::kFactorySetlist.data(), static_cast<qsizetype>(qc::view::kFactorySetlist.size())));
    refresh_ = new QPushButton(QStringLiteral("List"));
    scan_ = new QPushButton(QStringLiteral("Folders"));
    scan_->setToolTip(QStringLiteral("Enumerate every folder the device knows (takes ~12 s)"));
    top->addWidget(folders_, 1);
    top->addWidget(refresh_);
    top->addWidget(scan_);
    root->addLayout(top);

    list_ = new QListWidget;
    root->addWidget(list_, 1);
    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral("color: #8a8a90;"));
    root->addWidget(status_);

    auto* saveBox = new QGroupBox(QStringLiteral("Save As (snapshots the live grid)"));
    auto* saveLayout = new QVBoxLayout(saveBox);
    auto* l1 = new QHBoxLayout;
    saveSlot_ = new QLineEdit;
    saveSlot_->setPlaceholderText(QStringLiteral("slot, e.g. 30A"));
    saveSlot_->setMaximumWidth(90);
    saveName_ = new QLineEdit;
    saveName_->setPlaceholderText(QStringLiteral("preset name"));
    l1->addWidget(saveSlot_);
    l1->addWidget(saveName_, 1);
    auto* l2 = new QHBoxLayout;
    instrument_ = new QComboBox;
    instrument_->addItems({QStringLiteral("none"), QStringLiteral("Guitar"), QStringLiteral("Bass"), QStringLiteral("Synth"), QStringLiteral("Vocal"), QStringLiteral("Other")});
    save_ = new QPushButton(QStringLiteral("Save"));
    l2->addWidget(instrument_, 1);
    l2->addWidget(save_);
    saveLayout->addLayout(l1);
    saveLayout->addLayout(l2);
    root->addWidget(saveBox);

    connect(refresh_, &QPushButton::clicked, this, [this] { emit refreshRequested(currentFolder()); });
    connect(scan_, &QPushButton::clicked, this, [this] { emit foldersRequested(); });
    connect(folders_, &QComboBox::currentIndexChanged, this, [this](int) {
        list_->clear();
        emit refreshRequested(currentFolder());
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit recallRequested(currentFolder(), item->data(Qt::UserRole).toInt());
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        if (item && saveSlot_->text().isEmpty()) saveSlot_->setPlaceholderText(item->data(Qt::UserRole + 1).toString());
    });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, &LibraryPanel::showContextMenu);
    connect(save_, &QPushButton::clicked, this, [this] {
        auto pos = qc::view::slot_to_position(saveSlot_->text().toStdString());
        if (!pos) {
            status_->setText(QStringLiteral("slot: %1").arg(QString::fromStdString(pos.error())));
            return;
        }
        if (saveName_->text().trimmed().isEmpty()) {
            status_->setText(QStringLiteral("give the preset a name"));
            return;
        }
        emit saveRequested(currentFolder(), *pos, saveName_->text().trimmed(), instrument_->currentIndex());
    });
    setEnabledAll(false);
}

QString LibraryPanel::currentFolder() const { return folders_->currentData().toString(); }

void LibraryPanel::setEnabledAll(bool on) {
    for (auto* w : {static_cast<QWidget*>(folders_), static_cast<QWidget*>(list_), static_cast<QWidget*>(refresh_),
                    static_cast<QWidget*>(scan_), static_cast<QWidget*>(saveSlot_), static_cast<QWidget*>(saveName_),
                    static_cast<QWidget*>(instrument_), static_cast<QWidget*>(save_)}) {
        w->setEnabled(on);
    }
}

void LibraryPanel::setFolders(const QVector<qc::Folder>& folders) {
    const QString keep = currentFolder();
    folders_->blockSignals(true);
    folders_->clear();
    folders_->addItem(QStringLiteral("My Presets"), QString::fromUtf8(qc::view::kUserSetlist.data(), static_cast<qsizetype>(qc::view::kUserSetlist.size())));
    folders_->addItem(QStringLiteral("Factory Library"), QString::fromUtf8(qc::view::kFactorySetlist.data(), static_cast<qsizetype>(qc::view::kFactorySetlist.size())));
    factoryKeys_.clear();
    for (const auto& f : folders) {
        if (f.file_count == 0) continue;
        if (f.is_factory) factoryKeys_.insert(QString::fromStdString(f.key));
        if (qc::view::same_folder(f.key, qc::view::kUserSetlist) || qc::view::same_folder(f.key, qc::view::kFactorySetlist)) continue;
        const QString name = f.name.empty() ? QString::fromStdString(f.key) : QString::fromStdString(f.name);
        folders_->addItem(QStringLiteral("%1 (%2)").arg(name).arg(f.file_count), QString::fromStdString(f.key));
    }
    const int idx = folders_->findData(keep);
    folders_->setCurrentIndex(idx >= 0 ? idx : 0);
    folders_->blockSignals(false);
    status_->setText(QStringLiteral("%1 folders").arg(folders_->count()));
}

void LibraryPanel::setPresets(const QString& folder, const QVector<qc::PresetEntry>& presets) {
    if (!qc::view::same_folder(folder.toStdString(), currentFolder().toStdString())) return;
    list_->clear();
    for (const auto& p : presets) {
        auto* item = new QListWidgetItem(QStringLiteral("%1  %2").arg(QString::fromStdString(p.slot), -4).arg(QString::fromStdString(p.name)));
        item->setData(Qt::UserRole, p.index);
        item->setData(Qt::UserRole + 1, QString::fromStdString(p.slot));
        item->setData(Qt::UserRole + 2, QString::fromStdString(p.name));
        item->setToolTip(QStringLiteral("%1 — %2").arg(QString::fromStdString(p.author), QString::fromStdString(p.key)));
        list_->addItem(item);
    }
    status_->setText(QStringLiteral("%1 presets").arg(presets.size()));
}

void LibraryPanel::setCurrent(const QString& folder, int position) {
    if (!qc::view::same_folder(folder.toStdString(), currentFolder().toStdString())) return;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toInt() == position) {
            list_->setCurrentRow(i);
            return;
        }
    }
}

bool LibraryPanel::currentIsFactory() const {
    const QString key = currentFolder();
    if (qc::view::same_folder(key.toStdString(), qc::view::kFactorySetlist)) return true;
    return factoryKeys_.contains(key);
}

void LibraryPanel::showContextMenu(const QPoint& pos) {
    QListWidgetItem* item = list_->itemAt(pos);
    if (!item) return;
    const int position = item->data(Qt::UserRole).toInt();
    const QString slot = item->data(Qt::UserRole + 1).toString();
    const QString name = item->data(Qt::UserRole + 2).toString();
    const QString folder = currentFolder();

    QMenu menu(this);
    menu.addAction(QStringLiteral("Recall %1").arg(slot), this, [this, folder, position] { emit recallRequested(folder, position); });

    // Loeschen/Verschieben nur im eigenen (schreibbaren) Ordner und nur mit
    // bekanntem Namen - delete_preset/move_preset adressieren ueber den Namen.
    if (!currentIsFactory() && !name.isEmpty()) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Move \"%1\" to slot…").arg(name), this, [this, folder, name, slot] {
            bool ok = false;
            const QString target = QInputDialog::getText(this, QStringLiteral("Move preset"),
                                                         QStringLiteral("New slot for \"%1\" (e.g. 30A):").arg(name),
                                                         QLineEdit::Normal, slot, &ok);
            if (!ok) return;
            auto p = qc::view::slot_to_position(target.trimmed().toStdString());
            if (!p) {
                status_->setText(QStringLiteral("slot: %1").arg(QString::fromStdString(p.error())));
                return;
            }
            emit movePresetRequested(folder, name, *p);
        });
        menu.addAction(QStringLiteral("Delete \"%1\"…").arg(name), this, [this, folder, name] {
            const auto choice = QMessageBox::warning(
                this, QStringLiteral("Delete preset?"),
                QStringLiteral("Delete \"%1\" from the unit? This cannot be undone.").arg(name),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice == QMessageBox::Yes) emit deletePresetRequested(folder, name);
        });
    }
    menu.exec(list_->mapToGlobal(pos));
}
