// catalog.hpp — der Block-Katalog des Geraets (ModelRepo)
//
// Wer:      Martin.Lonkwitz
// Was:      Parst die ModelRepo-Payload gzip(tar(ModelRepo.xml)) in einen
//           Katalog: Modell-ID -> Name, Kategorie, Parameter in Wire-Index-
//           Reihenfolge mit min/max/default/units/type/steps. Loest dabei
//           `clones`/`replaces` auf: Ein Modell nennt oft nur die Parameter,
//           die es aendert, und erbt den Rest (Beleg 114/115).
// Wann:     2026-09-04
// Warum:    Ein Grid-Block ist auf dem Draht nur eine Zahl (Model.hash); die
//           Bedeutung von Parameter-Index N kennt nur dieser Katalog. Er kommt
//           VOM GERAET und deckt daher auch gekaufte Plugins/Captures ab.
// Projekt:  Ampersand
// Abhaengig: compression (zlib), tinyxml2
// Nutzung:  auto cat = qc::catalog::parse_model_repo(payload);
//           if (auto* m = cat->find(21005)) std::println("{}", m->name);
// Historie: git log
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qc::catalog {

struct Parameter {
    int index{0};
    std::string name;
    float minimum{0.0f};
    float maximum{1.0f};
    float default_value{0.0f};
    std::string units;
    std::string type;  ///< float, comboBox, switch, rotarySwitch, toggleButton, meter, grMeter, string, empty, ...
    std::optional<int> steps;
    /// Index, den dieser Parameter in der GEERBTEN Liste ersetzt (Attribut
    /// `replaces` am <Parameter>). Nur bei Modellen mit `clones` gesetzt; nach
    /// Catalog::finalize() ist die Liste aufgeloest und `index` gilt.
    /// Steht ABSICHTLICH am Ende: davor eingefuegt, bricht es jede
    /// aggregierte Initialisierung im Bestand.
    std::optional<int> replaces_index;

    /// "0..1 with a real-world unit" is the wire's own scale, not a span.
    [[nodiscard]] bool range_is_placeholder() const noexcept;
    /// Option count for list-valued parameters (wire value = N / (count-1)).
    [[nodiscard]] std::optional<int> option_count() const noexcept;
    [[nodiscard]] bool is_meter() const noexcept;
    /// Convert real units -> wire 0..1 (clamped). Placeholder ranges pass through.
    [[nodiscard]] float to_normalized(float real) const noexcept;
    [[nodiscard]] float to_real(float normalized) const noexcept;
};

struct Model {
    int id{0};
    std::string name;
    std::string category;
    int category_id{-1};
    std::string based_on;  ///< the "tm" attribute (real-world gear)
    std::vector<Parameter> parameters;
    std::optional<std::string> sku;
    std::optional<std::string> plugin_id;
    bool hidden{false};
    bool internal{false};
    bool category_hidden{false};
    /// Modell-IDs, die dieses Modell ersetzt (Attribut `replaces` am <Model>).
    /// NICHT zu verwechseln mit Parameter::replaces_index.
    std::vector<int> replaces;
    /// Modell, von dem die Parameterliste geerbt wird (Attribut `clones`).
    /// Ein solches Modell nennt selbst nur die Parameter, die es aendert.
    std::optional<int> clones;
    bool superseded{false};

    [[nodiscard]] bool is_capture_block() const noexcept { return category_id == 14 || category_id == 20; }
    [[nodiscard]] bool is_factory() const noexcept;
    [[nodiscard]] const Parameter* parameter(std::string_view name) const noexcept;
    [[nodiscard]] const Parameter* parameter(int index) const noexcept;
};

class Catalog {
public:
    [[nodiscard]] const Model* find(int id) const noexcept;
    [[nodiscard]] const Model* find_by_name(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> categories() const;
    [[nodiscard]] std::vector<const Model*> by_category(std::string_view category) const;
    [[nodiscard]] std::vector<const Model*> search(std::string_view needle) const;
    [[nodiscard]] std::size_t size() const noexcept { return models_.size(); }
    [[nodiscard]] const std::map<int, Model>& models() const noexcept { return models_; }
    void add(Model m);
    /// Zweiter Durchgang: geklonte Parameterlisten aufloesen und ersetzte
    /// Modelle markieren. MUSS nach dem Einlesen laufen - vorher stimmen die
    /// Parameter-Indizes geklonter Modelle nicht.
    void finalize();

private:
    void resolve_clones();

    std::map<int, Model> models_;
};

/// Extract ModelRepo.xml from gzip(tar(xml)), tar(xml), or bare xml.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> extract_xml(std::span<const std::uint8_t> payload);

/// Parse a ModelRepo payload (any of the three container forms) into a Catalog.
[[nodiscard]] std::expected<Catalog, std::string> parse_model_repo(std::span<const std::uint8_t> payload);

/// Read one member out of a (uncompressed) ustar archive.
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string>
tar_extract(std::span<const std::uint8_t> tar, std::string_view name_suffix);

}  // namespace qc::catalog
