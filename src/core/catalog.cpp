// catalog.cpp — ModelRepo-Parser (siehe catalog.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include "qc/catalog.hpp"

#include "qc/compression.hpp"
#include "qc/log.hpp"

#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace qc::catalog {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool icontains(std::string_view hay, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
    return it != hay.end();
}

float attr_float(const tinyxml2::XMLElement* e, const char* name, float fallback) {
    const char* v = e->Attribute(name);
    if (!v) return fallback;
    float out = fallback;
    const std::string_view sv(v);
    if (std::from_chars(sv.data(), sv.data() + sv.size(), out).ec != std::errc{}) return fallback;
    return out;
}

std::optional<int> attr_int(const tinyxml2::XMLElement* e, const char* name) {
    const char* v = e->Attribute(name);
    if (!v) return std::nullopt;
    int out = 0;
    const std::string_view sv(v);
    if (std::from_chars(sv.data(), sv.data() + sv.size(), out).ec != std::errc{}) return std::nullopt;
    return out;
}

std::string attr_str(const tinyxml2::XMLElement* e, const char* name) {
    const char* v = e->Attribute(name);
    return v ? std::string(v) : std::string{};
}

std::vector<int> parse_replaces(std::string_view v) {
    std::vector<int> out;
    while (!v.empty()) {
        const auto comma = v.find(',');
        auto part = v.substr(0, comma);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        int id = 0;
        if (!part.empty() && std::from_chars(part.data(), part.data() + part.size(), id).ec == std::errc{}) out.push_back(id);
        if (comma == std::string_view::npos) break;
        v.remove_prefix(comma + 1);
    }
    return out;
}

}  // namespace

// ── Parameter / Model ───────────────────────────────────────────────────────

bool Parameter::range_is_placeholder() const noexcept { return minimum == 0.0f && maximum == 1.0f && !units.empty(); }

std::optional<int> Parameter::option_count() const noexcept {
    if ((type == "comboBox" || type == "switch" || type == "rotarySwitch" || type == "empty") && steps && *steps > 0) {
        return steps;
    }
    return std::nullopt;
}

bool Parameter::is_meter() const noexcept {
    return type == "meter" || type == "grMeter" || type == "stereoMeter" || type == "stereoGrMeter";
}

float Parameter::to_normalized(float real) const noexcept {
    if (range_is_placeholder()) return std::clamp(real, 0.0f, 1.0f);
    const float span = maximum - minimum;
    if (span == 0.0f) return 0.0f;
    return std::clamp((real - minimum) / span, 0.0f, 1.0f);
}

float Parameter::to_real(float normalized) const noexcept {
    if (range_is_placeholder()) return std::clamp(normalized, 0.0f, 1.0f);
    const float span = maximum - minimum;
    if (span == 0.0f) return minimum;
    return minimum + std::clamp(normalized, 0.0f, 1.0f) * span;
}

bool Model::is_factory() const noexcept {
    return !(sku || plugin_id || hidden || internal || category_hidden || is_capture_block());
}

const Parameter* Model::parameter(std::string_view pname) const noexcept {
    for (const auto& p : parameters) {
        if (iequals(p.name, pname)) return &p;
    }
    return nullptr;
}

const Parameter* Model::parameter(int index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= parameters.size()) return nullptr;
    return &parameters[static_cast<std::size_t>(index)];
}

// ── Catalog ─────────────────────────────────────────────────────────────────

const Model* Catalog::find(int id) const noexcept {
    const auto it = models_.find(id);
    return it == models_.end() ? nullptr : &it->second;
}

const Model* Catalog::find_by_name(std::string_view name) const noexcept {
    for (const auto& [id, m] : models_) {
        if (iequals(m.name, name)) return &m;
    }
    return nullptr;
}

std::vector<std::string> Catalog::categories() const {
    std::vector<std::string> out;
    for (const auto& [id, m] : models_) {
        if (std::find(out.begin(), out.end(), m.category) == out.end()) out.push_back(m.category);
    }
    return out;
}

std::vector<const Model*> Catalog::by_category(std::string_view category) const {
    std::vector<const Model*> out;
    for (const auto& [id, m] : models_) {
        if (iequals(m.category, category)) out.push_back(&m);
    }
    return out;
}

std::vector<const Model*> Catalog::search(std::string_view needle) const {
    std::vector<const Model*> out;
    for (const auto& [id, m] : models_) {
        if (icontains(m.name, needle) || icontains(m.category, needle) || icontains(m.based_on, needle)) out.push_back(&m);
    }
    return out;
}

void Catalog::add(Model m) {
    const int id = m.id;
    models_.insert_or_assign(id, std::move(m));
}

namespace {

void renumber(std::vector<Parameter>& liste) {
    for (std::size_t i = 0; i < liste.size(); ++i) liste[i].index = static_cast<int>(i);
}

/// Effektive Parameterliste eines Modells: die (selbst schon aufgeloeste)
/// Liste des geklonten Modells, in der jeder eigene Parameter an SEINER Stelle
/// steht (`replaces="N"`). Eigene Parameter ohne `replaces` haengen hinten an.
///
/// Warum das sein muss: Modell 12050 "Gojira Cab 1 (M)" nennt selbst nur vier
/// Parameter, mit replaces 1, 9, 4 und 12 - die beiden Mikrofone und die beiden
/// DISTANCE-Werte. Ohne Aufloesung landen sie auf den Indizes 0..3, und damit
/// heisst auf dem Draht alles falsch (Beleg 115: am Display sitzen die
/// Mikrofone wirklich auf 1 und 9, DISTANCE auf 4 und 12).
std::vector<Parameter> aufgeloest(int id, std::map<int, Model>& modelle,
                                  std::map<int, std::vector<Parameter>>& fertig,
                                  std::vector<int>& unterwegs) {
    if (const auto it = fertig.find(id); it != fertig.end()) return it->second;
    const auto mi = modelle.find(id);
    if (mi == modelle.end()) return {};
    Model& m = mi->second;

    const bool zyklus = std::find(unterwegs.begin(), unterwegs.end(), id) != unterwegs.end();
    if (!m.clones || *m.clones == id || zyklus) {
        if (zyklus) log::warn("catalog: model {} clones in a cycle - own parameter list kept", id);
        auto liste = m.parameters;
        renumber(liste);
        return fertig.emplace(id, std::move(liste)).first->second;
    }

    unterwegs.push_back(id);
    auto liste = aufgeloest(*m.clones, modelle, fertig, unterwegs);
    unterwegs.pop_back();

    if (liste.empty() && !modelle.contains(*m.clones)) {
        log::warn("catalog: model {} clones {}, which is not in the repo", id, *m.clones);
    }

    for (const auto& eigen : m.parameters) {
        if (!eigen.replaces_index) {
            liste.push_back(eigen);
            continue;
        }
        const int n = *eigen.replaces_index;
        if (n >= 0 && static_cast<std::size_t>(n) < liste.size()) {
            liste[static_cast<std::size_t>(n)] = eigen;
        } else {
            // Nicht verwerfen: anhaengen und sagen, dass etwas nicht stimmt.
            log::warn("catalog: model {} replaces parameter {}, but the inherited list has only {}",
                      id, n, liste.size());
            liste.push_back(eigen);
        }
    }
    renumber(liste);
    return fertig.emplace(id, std::move(liste)).first->second;
}

}  // namespace

void Catalog::resolve_clones() {
    std::map<int, std::vector<Parameter>> fertig;
    std::vector<int> unterwegs;
    for (const auto& [id, m] : models_) {
        if (m.clones) (void)aufgeloest(id, models_, fertig, unterwegs);
    }
    for (auto& [id, m] : models_) {
        if (const auto it = fertig.find(id); it != fertig.end()) m.parameters = it->second;
    }
}

void Catalog::finalize() {
    resolve_clones();
    std::vector<int> replaced;
    for (const auto& [id, m] : models_) replaced.insert(replaced.end(), m.replaces.begin(), m.replaces.end());
    for (int old : replaced) {
        if (auto it = models_.find(old); it != models_.end()) it->second.superseded = true;
    }
}

// ── containers ──────────────────────────────────────────────────────────────

std::expected<std::vector<std::uint8_t>, std::string> tar_extract(std::span<const std::uint8_t> tar,
                                                                   std::string_view name_suffix) {
    constexpr std::size_t kBlock = 512;
    std::size_t off = 0;
    while (off + kBlock <= tar.size()) {
        const auto hdr = tar.subspan(off, kBlock);
        if (std::all_of(hdr.begin(), hdr.end(), [](std::uint8_t b) { return b == 0; })) break;  // end marker
        std::string name(reinterpret_cast<const char*>(hdr.data()), 100);  // unsafe{} tar header is plain bytes
        name.resize(std::min(name.size(), name.find('\0')));
        // ustar prefix (offset 345, 155 bytes)
        if (hdr.size() >= 500 && std::string_view(reinterpret_cast<const char*>(hdr.data() + 257), 5) == "ustar") {  // unsafe{}
            std::string prefix(reinterpret_cast<const char*>(hdr.data() + 345), 155);  // unsafe{}
            prefix.resize(std::min(prefix.size(), prefix.find('\0')));
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        std::size_t size = 0;
        for (std::size_t i = 124; i < 136; ++i) {
            const auto c = hdr[i];
            if (c < '0' || c > '7') break;
            size = size * 8 + static_cast<std::size_t>(c - '0');
        }
        const auto typeflag = hdr[156];
        const std::size_t data_off = off + kBlock;
        if (data_off + size > tar.size()) return std::unexpected("tar: member extends past end of archive");
        if ((typeflag == '0' || typeflag == 0) && name.size() >= name_suffix.size() &&
            name.compare(name.size() - name_suffix.size(), name_suffix.size(), name_suffix) == 0) {
            const auto data = tar.subspan(data_off, size);
            return std::vector<std::uint8_t>(data.begin(), data.end());
        }
        off = data_off + ((size + kBlock - 1) / kBlock) * kBlock;
    }
    return std::unexpected(std::format("tar: no member ending in '{}'", name_suffix));
}

std::expected<std::vector<std::uint8_t>, std::string> extract_xml(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> data;
    if (compression::is_gzip(payload)) {
        auto inflated = compression::inflate(payload);
        if (!inflated) return std::unexpected(inflated.error());
        data = std::move(*inflated);
    } else {
        data.assign(payload.begin(), payload.end());
    }
    auto first = std::find_if(data.begin(), data.end(), [](std::uint8_t b) { return !std::isspace(b); });
    if (first != data.end() && *first == '<') return data;
    return tar_extract(data, ".xml");
}

std::expected<Catalog, std::string> parse_model_repo(std::span<const std::uint8_t> payload) {
    auto xml = extract_xml(payload);
    if (!xml) return std::unexpected(xml.error());

    tinyxml2::XMLDocument doc;
    if (doc.Parse(reinterpret_cast<const char*>(xml->data()), xml->size()) != tinyxml2::XML_SUCCESS) {  // unsafe{}
        return std::unexpected(std::format("ModelRepo.xml: {}", doc.ErrorStr() ? doc.ErrorStr() : "parse error"));
    }
    const auto* root = doc.RootElement();
    if (!root) return std::unexpected("ModelRepo.xml: no root element");

    Catalog cat;
    for (const auto* category = root->FirstChildElement("Category"); category;
         category = category->NextSiblingElement("Category")) {
        const int category_id = attr_int(category, "id").value_or(-1);
        const std::string category_name = attr_str(category, "name");
        const bool category_hidden = category->Attribute("hidden") != nullptr;
        for (const auto* el = category->FirstChildElement("Model"); el; el = el->NextSiblingElement("Model")) {
            const auto id = attr_int(el, "id");
            if (!id) continue;
            Model m;
            m.id = *id;
            m.name = attr_str(el, "name");
            m.category = category_name;
            m.category_id = category_id;
            m.based_on = attr_str(el, "tm");
            if (el->Attribute("sku")) m.sku = attr_str(el, "sku");
            if (el->Attribute("plugin_id")) m.plugin_id = attr_str(el, "plugin_id");
            m.hidden = el->Attribute("hidden") != nullptr;
            m.internal = el->Attribute("internal") != nullptr;
            m.category_hidden = category_hidden;
            m.replaces = parse_replaces(attr_str(el, "replaces"));
            m.clones = attr_int(el, "clones");
            int index = 0;
            for (const auto* p = el->FirstChildElement("Parameter"); p; p = p->NextSiblingElement("Parameter")) {
                Parameter prm;
                prm.index = index++;
                prm.name = attr_str(p, "name");
                prm.minimum = attr_float(p, "min", 0.0f);
                prm.maximum = attr_float(p, "max", 1.0f);
                prm.default_value = attr_float(p, "defaultValue", 0.0f);
                prm.units = attr_str(p, "units");
                prm.type = attr_str(p, "type");
                prm.steps = attr_int(p, "steps");
                prm.replaces_index = attr_int(p, "replaces");
                m.parameters.push_back(std::move(prm));
            }
            cat.add(std::move(m));
        }
    }
    cat.finalize();
    if (cat.size() == 0) return std::unexpected("ModelRepo.xml: no models found");
    return cat;
}

}  // namespace qc::catalog
