// pitch.hpp — Tonhoehe aus einem Signalausschnitt bestimmen (YIN)
//
// Wer:      Martin.Lonkwitz
// Was:      Grundfrequenz eines monophonen Ausschnitts samt Guetemass, dazu die
//           Umrechnung in Notennamen und Cent-Abweichung.
// Wann:     2026-09-11
// Warum:    Das Geraet meldet dem Host weder Tonhoehe noch Pegel - beides steht
//           aber im USB-Audiostrom (docs/evidence/85). Damit laesst sich ein
//           eigenes Stimmgeraet bauen, das am Geraet gegengeprueft wurde:
//           +3,2 Cent gemessen bei +3.2 auf dem Display.
//           YIN statt einfacher Autokorrelation, weil diese bei Gitarrensignal
//           regelmaessig eine Quinte zu hoch liegt - im Versuch wurde 62 Hz als
//           93 Hz gelesen.
// Projekt:  Ampersand
// Abhaengig: nur die Standardbibliothek
// Nutzung:  auto r = qc::pitch::detect(samples, 48000);
//           if (r.reliable()) { auto n = qc::pitch::nearest_note(r.frequency_hz); }
// Historie: git log
#pragma once

#include <span>
#include <string>

namespace qc::pitch {

/// Ergebnis einer Tonhoehenmessung.
struct Reading {
    double frequency_hz{0.0};
    /// YIN-Aperiodizitaet: 0 = ideal periodisch, 1 = kein Ton erkennbar.
    /// Der Wert entscheidet, ob die Messung taugt — am Geraet lagen brauchbare
    /// Fenster bei 0,001 bis 0,016, unbrauchbare bei 0,1 bis 0,5.
    double aperiodicity{1.0};
    /// Effektivwert des Ausschnitts, damit Stille nicht als Ton gilt.
    double rms{0.0};

    [[nodiscard]] bool reliable(double limit = 0.05) const noexcept {
        return frequency_hz > 0.0 && aperiodicity <= limit;
    }
};

/// Note samt Abweichung zur temperierten Stimmung.
struct Note {
    std::string name;      ///< z. B. "B1"
    int midi{0};
    double cents{0.0};     ///< positiv = zu hoch
};

/// Grundfrequenz eines Ausschnitts. `samples` ist ein einzelner Kanal.
///
/// Vor der Analyse laeuft ein Tiefpass. Ohne ihn kaempft das Verfahren mit den
/// Obertoenen einer Gitarre: Gemessen wurde eine Guete zwischen 0,23 und 0,51,
/// obwohl der Ton sauber anlag - die staerkste Spektrallinie war dabei der
/// vierte Oberton, nicht der Grundton. Stimmgeraete filtern aus demselben
/// Grund.
/// Der Suchbereich deckt Bass bis hohe Lage ab; er begrenzt zugleich den
/// Rechenaufwand, der linear mit der groessten gesuchten Periode waechst.
[[nodiscard]] Reading detect(std::span<const float> samples, int sample_rate, double fmin_hz = 50.0,
                             double fmax_hz = 1200.0);

/// Naechstgelegene Note zu einer Frequenz, bezogen auf den Kammerton `a4_hz`.
[[nodiscard]] Note nearest_note(double frequency_hz, double a4_hz = 440.0);

}  // namespace qc::pitch
