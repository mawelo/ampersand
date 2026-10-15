// theme.hpp — dunkles Erscheinungsbild der GUI (Farben, Stylesheet)
//
// Wer:      Martin.Lonkwitz
// Was:      Farbpalette fuer Block-Kategorien, Szenenfarben und das Qt-
//           Stylesheet, angelehnt an Cortex Control (dunkler Grid-Hintergrund,
//           farbige Block-Rahmen).
// Wann:     2026-09-04
// Projekt:  Ampersand (GUI)
// Historie: git log
#pragma once

#include <QColor>
#include <QString>

namespace theme {

inline const QColor kBackground{0x14, 0x14, 0x16};
inline const QColor kGridLine{0x4a, 0x4a, 0x50};
inline const QColor kEmptyCell{0x2a, 0x2a, 0x2e};
inline const QColor kText{0xe6, 0xe6, 0xe6};
inline const QColor kTextDim{0x8a, 0x8a, 0x90};
inline const QColor kSelected{0xff, 0xff, 0xff};
inline const QColor kNode{0x8b, 0x7c, 0xff};  // IN / OUT nodes (violet like the reference)
inline const QColor kAccent{0xf5, 0xc4, 0x51};  // dasselbe Gelb wie im Stylesheet
inline const QColor kOk{0x62, 0xd2, 0x7a};      // "stimmt" - etwa beim Stimmgeraet

/// Border colour for a block by its catalog category name.
inline QColor category_color(const QString& category) {
    const QString c = category.toLower();
    if (c.contains("capture")) return {0xff, 0x5a, 0x3c};                        // red-orange (unit)
    if (c.contains("pitch")) return {0xff, 0xd8, 0x3d};                          // yellow (unit)
    if (c.contains("equalizer") || c == "eq") return {0x3f, 0xd0, 0xff};         // cyan (unit)
    if (c.contains("cab") || c.contains("irloader")) return {0x4a, 0x7d, 0xff};  // blue (unit)
    if (c.contains("amp")) return {0xff, 0x8c, 0x42};                            // orange
    if (c.contains("overdrive") || c.contains("dist") || c.contains("fuzz")) return {0xff, 0x6a, 0x6a};
    if (c.contains("delay")) return {0x62, 0xd2, 0x7a};
    if (c.contains("reverb")) return {0x3f, 0xb8, 0xa8};
    if (c.contains("mod")) return {0xc0, 0x7c, 0xff};
    if (c.contains("comp") || c.contains("dyn")) return {0xf0, 0x7a, 0xd0};
    if (c.contains("wah") || c.contains("filter")) return {0xc8, 0xff, 0x6a};
    if (c.contains("synth")) return {0xe0, 0x60, 0xff};
    if (c.contains("loop")) return {0xa0, 0xa0, 0xa8};
    if (c.contains("utility")) return {0xd8, 0xe8, 0xf0};                        // near white (gate on the unit)
    return {0x9a, 0x9a, 0xa5};
}

inline QString stylesheet() {
    return QStringLiteral(R"(
QMainWindow, QWidget { background: #141416; color: #e6e6e6; font-size: 12px; }
QToolBar { background: #1c1c20; border: 0; spacing: 6px; padding: 4px; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: #1c1c20; padding: 6px; font-weight: bold; }
QPushButton, QToolButton { background: #26262b; border: 1px solid #3a3a40; border-radius: 6px; padding: 5px 10px; color: #e6e6e6; }
QPushButton:hover, QToolButton:hover { background: #33333a; }
QPushButton:pressed, QToolButton:pressed { background: #1a1a1e; }
QPushButton:disabled, QToolButton:disabled { color: #6a6a70; border-color: #2a2a2e; }
QToolButton:checked { background: #f5c451; color: #141416; font-weight: bold; }
QLineEdit, QComboBox, QSpinBox, QListWidget, QTextEdit, QPlainTextEdit { background: #1c1c20; border: 1px solid #3a3a40; border-radius: 4px; padding: 3px; color: #e6e6e6; selection-background-color: #f5c451; selection-color: #141416; }
QListWidget::item:selected { background: #f5c451; color: #141416; }
QSlider::groove:horizontal { height: 6px; background: #2a2a2e; border-radius: 3px; }
QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: #f5c451; border-radius: 7px; }
QSlider::sub-page:horizontal { background: #8b7cff; border-radius: 3px; }
QLabel#presetName { font-size: 18px; font-weight: bold; }
QLabel#presetSlot { font-size: 14px; color: #8a8a90; }
QStatusBar { background: #1c1c20; color: #8a8a90; }
QScrollBar:vertical { background: #1c1c20; width: 10px; }
QScrollBar::handle:vertical { background: #3a3a40; border-radius: 5px; min-height: 20px; }
QGroupBox { border: 1px solid #3a3a40; border-radius: 6px; margin-top: 10px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #8a8a90; }
QCheckBox::indicator { width: 14px; height: 14px; }
QMenuBar { background: #1c1c20; }
QMenu { background: #1c1c20; border: 1px solid #3a3a40; }
QMenu::item:selected { background: #33333a; }
)");
}

}  // namespace theme
