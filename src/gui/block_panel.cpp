// block_panel.cpp — Parameter-Editor (siehe block_panel.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#include "block_panel.hpp"

#include "theme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <string_view>

namespace {
constexpr int kSliderMax = 1000;

// ── Gruppen im Cab-Block ────────────────────────────────────────────────
// Das Geraet selbst fuehrt zwei Reiter: CAB (die beiden Mikrofonwege) und
// EQ+ROOM. Die Parameterliste kommt aber flach vom Geraet, und bei einem
// Cab-Block sind es einunddreissig - ohne Gliederung findet dort niemand
// etwas. Gruppiert wird nach NAMEN, nicht nach Index: dieselbe Vorlage
// (Modell 12100) steckt hinter 202 Modellen des Katalogs, und die Namen sind
// stabiler als jede Indexannahme.
bool ist_cab(std::string_view kategorie) { return kategorie.starts_with("Cabsim"); }

const char* gruppe_von(std::string_view name) {
    if (name == "HPF" || name == "LPF" || name == "OUTPUT VOLUME") return "EQ";
    if (name.starts_with("ROOM ") || name == "PRE DELAY" || name == "SIZE" ||
        name == "REV HI PASS" || name == "REV LOW PASS") {
        return "Room";
    }
    return nullptr;
}

// Gemessen, nicht vermutet (docs/evidence/116): Von den fuenf Raum-Parametern,
// fuer die das Geraet keinen Regler zeigt, wirken drei und zwei nicht.
bool ohne_wirkung(std::string_view name) { return name == "ROOM MIX" || name == "SIZE"; }
bool wirkt_ohne_regler(std::string_view name) {
    return name == "PRE DELAY" || name == "REV HI PASS" || name == "REV LOW PASS";
}

QLabel* gruppenkopf(const char* titel) {
    auto* l = new QLabel(QString::fromLatin1(titel).toUpper());
    l->setStyleSheet(QStringLiteral("color: #9a9aa2; font-weight: bold; "
                                    "border-top: 1px solid #3a3a42; padding-top: 6px; margin-top: 6px;"));
    return l;
}

QLabel* gruppenhinweis() {
    auto* l = new QLabel(QStringLiteral(
        "PRE DELAY, REV HI PASS and REV LOW PASS change the sound but have no "
        "control on the unit itself. ROOM MIX and SIZE measured as no-ops."));
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: #7a7a82; font-size: 11px;"));
    l->setToolTip(QStringLiteral(
        "Measured 2026-09-16 on model 12050 by reamping the same 30 s signal: "
        "PRE DELAY 1.03 dB, REV HI PASS 3.41 dB, REV LOW PASS 2.33 dB mean "
        "deviation across 27 third-octave bands, against a noise floor of "
        "0.08 dB. ROOM MIX 0.11 dB and SIZE 0.13 dB stayed inside that floor. "
        "See docs/evidence/116-raum-klanglich."));
    return l;
}
}  // namespace

BlockPanel::BlockPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    title_ = new QLabel(QStringLiteral("No block selected"));
    title_->setObjectName(QStringLiteral("presetName"));
    title_->setWordWrap(true);
    subtitle_ = new QLabel;
    subtitle_->setObjectName(QStringLiteral("presetSlot"));
    root->addWidget(title_);
    root->addWidget(subtitle_);

    auto* buttons = new QHBoxLayout;
    bypass_ = new QCheckBox(QStringLiteral("Bypass"));
    remove_ = new QPushButton(QStringLiteral("Remove"));
    add_ = new QPushButton(QStringLiteral("Add block…"));
    buttons->addWidget(bypass_);
    buttons->addStretch();
    buttons->addWidget(add_);
    buttons->addWidget(remove_);
    root->addLayout(buttons);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    paramsHost_ = new QWidget;
    paramsLayout_ = new QVBoxLayout(paramsHost_);
    paramsLayout_->setContentsMargins(0, 4, 0, 4);
    paramsLayout_->addStretch();
    scroll->setWidget(paramsHost_);
    root->addWidget(scroll, 1);

    throttle_ = new QTimer(this);
    throttle_->setSingleShot(true);
    throttle_->setInterval(30);
    connect(throttle_, &QTimer::timeout, this, [this] {
        if (pending_ && row_ >= 0) emit paramChanged(row_, column_, pending_->first, pending_->second);
        pending_.reset();
    });

    connect(bypass_, &QCheckBox::toggled, this, [this](bool on) {
        if (!suppress_ && row_ >= 0 && block_) emit bypassToggled(row_, column_, on);
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        if (row_ >= 0 && block_) emit removeRequested(row_, column_);
    });
    connect(add_, &QPushButton::clicked, this, [this] {
        if (row_ >= 0) emit addRequested(row_, column_);
    });
    showNothing();
}

void BlockPanel::clearParams() {
    params_.clear();  // the widgets themselves belong to the row containers deleted below
    while (paramsLayout_->count() > 1) {
        auto* item = paramsLayout_->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

QString BlockPanel::valueText(const qc::view::ParamView& p, float normalized) const {
    qc::view::ParamView tmp = p;
    tmp.value = normalized;
    return QString::fromStdString(tmp.display_value());
}

void BlockPanel::showNothing() {
    clearParams();
    block_.reset();
    row_ = column_ = -1;
    title_->setText(QStringLiteral("No block selected"));
    subtitle_->setText(QStringLiteral("Click a cell on the grid."));
    bypass_->setEnabled(false);
    remove_->setEnabled(false);
    add_->setEnabled(false);
}

void BlockPanel::showEmpty(int row, int column) {
    clearParams();
    block_.reset();
    row_ = row;
    column_ = column;
    title_->setText(QStringLiteral("Empty cell"));
    subtitle_->setText(QStringLiteral("Row %1, column %2").arg(row + 1).arg(column + 1));
    bypass_->setEnabled(false);
    remove_->setEnabled(false);
    add_->setEnabled(true);
}

void BlockPanel::showBlock(const qc::view::BlockView& block) {
    clearParams();
    block_ = block;
    row_ = block.row;
    column_ = block.column;
    title_->setText(QString::fromStdString(block.name));
    subtitle_->setText(QStringLiteral("%1 · id %2 · row %3 col %4%5")
                           .arg(QString::fromStdString(block.category))
                           .arg(block.model_id)
                           .arg(block.row + 1)
                           .arg(block.column + 1)
                           .arg(block.scene_mode_bypass ? QStringLiteral(" · scene bypass") : QString()));
    suppress_ = true;
    bypass_->setChecked(block.bypassed);
    suppress_ = false;
    bypass_->setEnabled(true);
    remove_->setEnabled(true);
    add_->setEnabled(true);

    const QColor color = theme::category_color(QString::fromStdString(block.category));
    title_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));

    const bool cab = ist_cab(block.category);
    const char* gruppe_aktiv = nullptr;
    for (const auto& p : block.params) {
        if (p.type == "meter" || p.type == "grMeter" || p.type == "stereoMeter" || p.type == "stereoGrMeter") continue;
        if (p.type == "empty" || p.type == "string") continue;

        // Gruppierung nur bei Cab-Bloecken: "SIZE" heisst in einem Reverb-Block
        // etwas anderes und wirkt dort, also wird dort nichts gruppiert und
        // nichts als wirkungslos ausgewiesen.
        if (cab) {
            if (const char* g = gruppe_von(p.name); g && g != gruppe_aktiv) {
                gruppe_aktiv = g;
                paramsLayout_->insertWidget(paramsLayout_->count() - 1, gruppenkopf(g));
                if (std::string_view(g) == "Room") {
                    paramsLayout_->insertWidget(paramsLayout_->count() - 1, gruppenhinweis());
                }
            }
        }

        auto* rowWidget = new QWidget;
        auto* rowLayout = new QVBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 2, 0, 2);
        rowLayout->setSpacing(2);
        auto* head = new QHBoxLayout;
        auto* name = new QLabel(QStringLiteral("%1%2").arg(QString::fromStdString(p.name), p.scene_mode ? QStringLiteral(" ◆") : QString()));
        name->setToolTip(p.scene_mode ? QStringLiteral("Scene-following parameter (value per scene)") : QString());
        if (cab && ohne_wirkung(p.name)) {
            // Gedaempft wie ein abgeschaltetes Bedienelement am Geraet - der
            // Regler bleibt benutzbar, denn "ohne Wirkung" heisst "unter dem
            // Rauschboden dieses Aufbaus", nicht "das DSP ignoriert es".
            name->setStyleSheet(QStringLiteral("color: #6a6a72;"));
            name->setToolTip(QStringLiteral(
                "No audible effect on model 12050: 0.11 dB (ROOM MIX) and 0.13 dB (SIZE) "
                "mean deviation across 27 third-octave bands, inside the 0.08 dB noise "
                "floor of two identical runs (docs/evidence/116). Other cab models share "
                "the same template 12100 but were not measured individually."));
        } else if (cab && wirkt_ohne_regler(p.name)) {
            name->setToolTip(QStringLiteral(
                "The unit shows no control for this parameter, but it does change the "
                "sound - measured on model 12050 (docs/evidence/116)."));
        }
        auto* value = new QLabel(QString::fromStdString(p.display_value()));
        value->setStyleSheet(QStringLiteral("color: #8a8a90;"));
        head->addWidget(name);
        head->addStretch();
        head->addWidget(value);
        rowLayout->addLayout(head);

        ParamWidgets w;
        w.index = p.index;
        w.spec = p;
        w.value = value;
        if (p.text) {
            auto* t = new QLabel(QString::fromStdString(*p.text));
            t->setWordWrap(true);
            t->setStyleSheet(QStringLiteral("color: #e6e6e6; font-style: italic;"));
            rowLayout->addWidget(t);
        } else if (!p.options.empty()) {
            auto* combo = new QComboBox;
            for (const auto& o : p.options) combo->addItem(QString::fromStdString(o));
            const int count = static_cast<int>(p.options.size());
            const int cur = p.value ? static_cast<int>(std::lround(*p.value * static_cast<float>(count - 1))) : 0;
            combo->setCurrentIndex(std::clamp(cur, 0, count - 1));
            w.combo = combo;
            rowLayout->addWidget(combo);
            const std::size_t slot = params_.size();
            connect(combo, &QComboBox::currentIndexChanged, this, [this, slot](int idx) {
                if (suppress_ || slot >= params_.size()) return;
                const auto& pw = params_[slot];
                const int n = pw.combo->count();
                const float v = n > 1 ? static_cast<float>(idx) / static_cast<float>(n - 1) : 0.0f;
                pw.value->setText(valueText(pw.spec, v));
                emit paramChanged(row_, column_, pw.index, v);
            });
        } else {
            auto* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, kSliderMax);
            slider->setValue(static_cast<int>(std::lround(p.value.value_or(0.0f) * kSliderMax)));
            w.slider = slider;
            rowLayout->addWidget(slider);
            const std::size_t slot = params_.size();
            connect(slider, &QSlider::valueChanged, this, [this, slot](int v) {
                if (suppress_ || slot >= params_.size()) return;
                const auto& pw = params_[slot];
                const float f = static_cast<float>(v) / kSliderMax;
                pw.value->setText(valueText(pw.spec, f));
                pending_ = std::make_pair(pw.index, f);
                throttle_->start();
            });
        }
        params_.push_back(w);
        paramsLayout_->insertWidget(paramsLayout_->count() - 1, rowWidget);
    }
}

void BlockPanel::update(const qc::view::PresetView& view) {
    if (row_ < 0 || !block_) return;
    const auto& rv = view.rows[static_cast<std::size_t>(row_)];
    const auto* b = rv.block_at(column_);
    if (!b) {
        showEmpty(row_, column_);
        return;
    }
    if (b->model_id != block_->model_id || b->params.size() != block_->params.size()) {
        showBlock(*b);
        return;
    }
    block_ = *b;
    suppress_ = true;
    bypass_->setChecked(b->bypassed);
    for (auto& w : params_) {
        const auto it = std::find_if(b->params.begin(), b->params.end(), [&](const auto& p) { return p.index == w.index; });
        if (it == b->params.end()) continue;
        w.spec = *it;
        if (w.slider && !w.slider->isSliderDown() && it->value) {
            w.slider->setValue(static_cast<int>(std::lround(*it->value * kSliderMax)));
        }
        if (w.combo && it->value) {
            const int n = w.combo->count();
            w.combo->setCurrentIndex(std::clamp(static_cast<int>(std::lround(*it->value * static_cast<float>(n - 1))), 0, n - 1));
        }
        w.value->setText(QString::fromStdString(it->display_value()));
    }
    suppress_ = false;
}

void BlockPanel::emitParam(const ParamWidgets& w) {
    if (w.slider) emit paramChanged(row_, column_, w.index, static_cast<float>(w.slider->value()) / kSliderMax);
}
