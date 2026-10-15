// pitch.cpp — YIN-Tonhoehenerkennung (siehe include/qc/pitch.hpp)
//
// Wer:      Martin.Lonkwitz
// Was:      Differenzfunktion, kumulative Normierung, Schwellenwahl und
//           parabolische Verfeinerung; dazu Notenname und Cent-Abweichung.
// Wann:     2026-09-11
// Warum:    siehe Kopf von include/qc/pitch.hpp
// Projekt:  Ampersand
// Abhaengig: nur die Standardbibliothek
// Nutzung:  ueber qc::pitch::detect / nearest_note
// Historie: git log
#include "qc/pitch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <vector>

namespace qc::pitch {

namespace {

constexpr std::array<const char*, 12> kNoteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/// Ueber diese Laenge hinaus bringt ein laengeres Fenster nichts mehr: Die
/// Differenzfunktion mittelt dann ueber Tonhoehenaenderungen hinweg, die bei
/// einer ausklingenden Saite tatsaechlich stattfinden (gemessen: 8 Cent in
/// 0,6 s auf einer lose gespannten Saite).
constexpr double kMaxWindowSeconds = 0.5;

/// Jeder zweite Abtastwert genuegt fuer die Differenzfunktion. Das halbiert den
/// Aufwand und aendert das Ergebnis nicht messbar, weil ueber viele Perioden
/// gemittelt wird.
constexpr int kStride = 2;

/// Eckfrequenz des Tiefpasses. Deckt Gitarre und Bass ab (die hoechste
/// gebraeuchliche Grundfrequenz liegt weit darunter), daempft aber die
/// Obertoene, die YIN sonst in die Irre fuehren.
constexpr double kLowpassHz = 900.0;

/// Einpoliger Tiefpass, zweimal angewandt (also 12 dB je Oktave). Mehr braucht
/// es nicht: Es geht nicht um Klang, sondern darum, dass die Differenzfunktion
/// den Grundton findet statt einen Oberton.
void lowpass(std::vector<double>& x, int sample_rate, double cutoff_hz) {
    if (x.empty() || sample_rate <= 0) return;
    const double dt = 1.0 / static_cast<double>(sample_rate);
    const double rc = 1.0 / (2.0 * std::numbers::pi * cutoff_hz);
    const double alpha = dt / (rc + dt);
    for (int pass = 0; pass < 2; ++pass) {
        double y = x.front();
        for (double& v : x) {
            y += alpha * (v - y);
            v = y;
        }
    }
}

}  // namespace

Reading detect(std::span<const float> samples, int sample_rate, double fmin_hz, double fmax_hz) {
    Reading result;
    if (sample_rate <= 0 || samples.empty() || fmin_hz <= 0.0 || fmax_hz <= fmin_hz) return result;

    const std::size_t max_len = static_cast<std::size_t>(kMaxWindowSeconds * sample_rate);
    if (samples.size() > max_len) samples = samples.subspan(0, max_len);

    const int tau_min = std::max(2, static_cast<int>(sample_rate / fmax_hz));
    const int tau_max = static_cast<int>(sample_rate / fmin_hz);
    // Unterhalb von zwei Perioden im Fenster ist die Differenzfunktion nicht
    // aussagekraeftig — dann lieber nichts melden als etwas Erfundenes.
    if (tau_max <= tau_min || samples.size() < static_cast<std::size_t>(2 * tau_max)) return result;

    // Gleichanteil entfernen; er verschiebt die Differenzfunktion sonst so, dass
    // lange Perioden bevorzugt werden.
    double mean = 0.0;
    for (const float v : samples) mean += static_cast<double>(v);
    mean /= static_cast<double>(samples.size());

    std::vector<double> centred(samples.size());
    double energy = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        centred[i] = static_cast<double>(samples[i]) - mean;
        energy += centred[i] * centred[i];
    }
    result.rms = std::sqrt(energy / static_cast<double>(samples.size()));
    if (result.rms <= 0.0) return result;

    // Der Pegel wird am ungefilterten Signal gemessen (er soll die Aussteuerung
    // zeigen), die Tonhoehe am gefilterten.
    lowpass(centred, sample_rate, kLowpassHz);

    const std::size_t span = samples.size() - static_cast<std::size_t>(tau_max);
    std::vector<double> diff(static_cast<std::size_t>(tau_max) + 1, 0.0);
    for (int tau = tau_min; tau <= tau_max; ++tau) {
        double sum = 0.0;
        for (std::size_t i = 0; i + static_cast<std::size_t>(tau) < span + static_cast<std::size_t>(tau);
             i += kStride) {
            if (i >= span) break;
            const double d = centred[i] - centred[i + static_cast<std::size_t>(tau)];
            sum += d * d;
        }
        diff[static_cast<std::size_t>(tau)] = sum;
    }

    // Kumulative mittlere normierte Differenz: Erst dadurch wird der Wert
    // vergleichbar und die Schwelle unten sinnvoll.
    std::vector<double> cumulative(diff.size(), 1.0);
    double running = 0.0;
    for (int tau = tau_min; tau <= tau_max; ++tau) {
        running += diff[static_cast<std::size_t>(tau)];
        cumulative[static_cast<std::size_t>(tau)] =
            running > 0.0 ? diff[static_cast<std::size_t>(tau)] * (tau - tau_min + 1) / running : 1.0;
    }

    // Erstes lokales Minimum unter der Schwelle gewinnt — nicht das globale.
    // Genau das verhindert den Griff zur Oktave oder Quinte darueber.
    constexpr double kThreshold = 0.15;
    int best = -1;
    for (int tau = tau_min + 1; tau < tau_max; ++tau) {
        const double v = cumulative[static_cast<std::size_t>(tau)];
        if (v < kThreshold && v <= cumulative[static_cast<std::size_t>(tau) - 1] &&
            v <= cumulative[static_cast<std::size_t>(tau) + 1]) {
            best = tau;
            break;
        }
    }
    if (best < 0) {
        const auto it = std::min_element(cumulative.begin() + tau_min, cumulative.begin() + tau_max);
        best = static_cast<int>(std::distance(cumulative.begin(), it));
    }
    if (best <= tau_min || best >= tau_max) return result;

    // Parabel durch die drei Werte um das Minimum: bringt die Aufloesung unter
    // einen Abtastwert, was bei tiefen Toenen ueber Cent-Genauigkeit entscheidet.
    const double y0 = cumulative[static_cast<std::size_t>(best) - 1];
    const double y1 = cumulative[static_cast<std::size_t>(best)];
    const double y2 = cumulative[static_cast<std::size_t>(best) + 1];
    const double denom = y0 - 2.0 * y1 + y2;
    const double shift = denom != 0.0 ? (y0 - y2) / (2.0 * denom) / 2.0 : 0.0;

    result.frequency_hz = static_cast<double>(sample_rate) / (static_cast<double>(best) + shift);
    result.aperiodicity = y1;
    return result;
}

Note nearest_note(double frequency_hz, double a4_hz) {
    Note note;
    if (frequency_hz <= 0.0 || a4_hz <= 0.0) return note;
    const double half_steps = 12.0 * std::log2(frequency_hz / a4_hz) + 69.0;
    note.midi = static_cast<int>(std::lround(half_steps));
    note.cents = (half_steps - note.midi) * 100.0;
    const int index = ((note.midi % 12) + 12) % 12;
    note.name = std::format("{}{}", kNoteNames[static_cast<std::size_t>(index)], note.midi / 12 - 1);
    return note;
}

}  // namespace qc::pitch
