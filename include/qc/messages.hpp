// messages.hpp — Nachrichtentypen und Aktionen des Quad-Cortex-Protokolls
//
// Wer:      Martin.Lonkwitz
// Was:      Typsichere Enums fuer CortexMessageType.Enum (71 Typen, Trailer-
//           Tag) und MessageAction.Enum, mit Namensauflösung in beide
//           Richtungen. Quelle: proto/ProductionAutomation.proto.
// Wann:     2026-09-04
// Warum:    Der Typ steht als nackte Zahl im Trailer; Session/CLI/GUI brauchen
//           sprechende Namen ohne Protobuf-Reflection.
// Projekt:  Ampersand
// Abhaengig: keine
// Nutzung:  qc::to_string(qc::MessageType::Grid) == "Grid"
// Historie: git log
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace qc {

enum class MessageType : std::uint16_t {
    Undefined = 0, Grid = 1, SetlistPosition = 2, IOSettings = 3, File = 4, IOMeter = 5,
    Tuner = 6, Diagnostics = 7, MIDISettings = 8, GeneralSettings = 9, Version = 10,
    ProductionAutomationMode = 11, GridMove = 12, Scene = 13, Mode = 14, RecallPreset = 15,
    EnableCaptureOut = 16, MasterVolume = 17, CloudLogin = 18, DefaultParameters = 19,
    RecentsFavorites = 20, UndoRedo = 21, SceneCopy = 22, SceneLabel = 23, ShowGigView = 24,
    Screenshot = 25, CPULoad = 26, ShowTuner = 27, Looper = 28, ProductForward = 29,
    BackupsForward = 30, LogsForward = 31, KeepAlive = 32, GlobalTempo = 33, PresetDirty = 34,
    ModuleStats = 35, NeuralCapture = 36, GridModelMeter = 37, GlobalEQ = 38,
    RecentSearches = 39, LocalBackup = 40, CloudBackup = 41, CompilerInhibitedModules = 42,
    SystemTimeSync = 43, Logs = 44, ProcessDownloadsQueue = 45, CloudProduct = 46,
    Confirmation = 47, SceneColor = 48, Connection = 49, NewModels = 50, ModelRepo = 51,
    ResetCommsBuffers = 52, SuspendConnection = 53, PinnedModels = 54, GigViewButton = 55,
    GenericError = 56, BulkOperation = 57, License = 58, PresetSpeedTest = 59, Updater = 60,
    UpdaterForward = 61, GainCalibration = 62, NeuralCapture2 = 63, Serialization = 64,
    TestFarm = 65, ProductionTest = 66, LoadAutomatedTestPreset = 67,
    SetTestPresetInputOutputPorts = 68, SetTestPresetSplitMixPoints = 69,
    GenerateTestPreset = 70,
    // Ab hier aus CorOS 4.1.0 zurueckgewonnen (docs/evidence/93). Unser
    // Protobuf stammte aus Cortex Control 4.0.1 und endete bei 70; das Geraet
    // kennt zwei Typen mehr, und einer davon lief uns in evidence/92 als
    // unbekannte Nachricht 71 ueber den Weg.
    ModelPreset = 71, RemoteControl = 72, NumberOfMessageTypes = 73,
};

enum class Action : std::uint8_t {
    Create = 0, Update = 1, Delete = 2, Read = 3, Move = 4, Copy = 5, Upload = 6, Download = 7, Swap = 8,
};

[[nodiscard]] std::string_view to_string(MessageType t) noexcept;
[[nodiscard]] std::string_view to_string(Action a) noexcept;
[[nodiscard]] std::optional<MessageType> parse_message_type(std::string_view name) noexcept;
[[nodiscard]] constexpr std::uint16_t raw(MessageType t) noexcept { return static_cast<std::uint16_t>(t); }
[[nodiscard]] MessageType message_type_from_raw(std::uint16_t v) noexcept;

}  // namespace qc
