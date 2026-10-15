// messages.cpp — Namen der Nachrichtentypen (siehe messages.hpp)
//
// Wer:      Martin.Lonkwitz
// Wann:     2026-09-04
// Projekt:  Ampersand
// Historie: git log
#include <algorithm>
#include "qc/messages.hpp"

#include <array>
#include <utility>

namespace qc {

namespace {

constexpr std::array<std::pair<MessageType, std::string_view>, 74> kNames{{
    {MessageType::Undefined, "Undefined"}, {MessageType::Grid, "Grid"},
    {MessageType::SetlistPosition, "SetlistPosition"}, {MessageType::IOSettings, "IOSettings"},
    {MessageType::File, "File"}, {MessageType::IOMeter, "IOMeter"}, {MessageType::Tuner, "Tuner"},
    {MessageType::Diagnostics, "Diagnostics"}, {MessageType::MIDISettings, "MIDISettings"},
    {MessageType::GeneralSettings, "GeneralSettings"}, {MessageType::Version, "Version"},
    {MessageType::ProductionAutomationMode, "ProductionAutomationMode"},
    {MessageType::GridMove, "GridMove"}, {MessageType::Scene, "Scene"}, {MessageType::Mode, "Mode"},
    {MessageType::RecallPreset, "RecallPreset"}, {MessageType::EnableCaptureOut, "EnableCaptureOut"},
    {MessageType::MasterVolume, "MasterVolume"}, {MessageType::CloudLogin, "CloudLogin"},
    {MessageType::DefaultParameters, "DefaultParameters"},
    {MessageType::RecentsFavorites, "RecentsFavorites"}, {MessageType::UndoRedo, "UndoRedo"},
    {MessageType::SceneCopy, "SceneCopy"}, {MessageType::SceneLabel, "SceneLabel"},
    {MessageType::ShowGigView, "ShowGigView"}, {MessageType::Screenshot, "Screenshot"},
    {MessageType::CPULoad, "CPULoad"}, {MessageType::ShowTuner, "ShowTuner"},
    {MessageType::Looper, "Looper"}, {MessageType::ProductForward, "ProductForward"},
    {MessageType::BackupsForward, "BackupsForward"}, {MessageType::LogsForward, "LogsForward"},
    {MessageType::KeepAlive, "KeepAlive"}, {MessageType::GlobalTempo, "GlobalTempo"},
    {MessageType::PresetDirty, "PresetDirty"}, {MessageType::ModuleStats, "ModuleStats"},
    {MessageType::NeuralCapture, "NeuralCapture"}, {MessageType::GridModelMeter, "GridModelMeter"},
    {MessageType::GlobalEQ, "GlobalEQ"}, {MessageType::RecentSearches, "RecentSearches"},
    {MessageType::LocalBackup, "LocalBackup"}, {MessageType::CloudBackup, "CloudBackup"},
    {MessageType::CompilerInhibitedModules, "CompilerInhibitedModules"},
    {MessageType::SystemTimeSync, "SystemTimeSync"}, {MessageType::Logs, "Logs"},
    {MessageType::ProcessDownloadsQueue, "ProcessDownloadsQueue"},
    {MessageType::CloudProduct, "CloudProduct"}, {MessageType::Confirmation, "Confirmation"},
    {MessageType::SceneColor, "SceneColor"}, {MessageType::Connection, "Connection"},
    {MessageType::NewModels, "NewModels"}, {MessageType::ModelRepo, "ModelRepo"},
    {MessageType::ResetCommsBuffers, "ResetCommsBuffers"},
    {MessageType::SuspendConnection, "SuspendConnection"}, {MessageType::PinnedModels, "PinnedModels"},
    {MessageType::GigViewButton, "GigViewButton"}, {MessageType::GenericError, "GenericError"},
    {MessageType::BulkOperation, "BulkOperation"}, {MessageType::License, "License"},
    {MessageType::PresetSpeedTest, "PresetSpeedTest"}, {MessageType::Updater, "Updater"},
    {MessageType::UpdaterForward, "UpdaterForward"}, {MessageType::GainCalibration, "GainCalibration"},
    {MessageType::NeuralCapture2, "NeuralCapture2"}, {MessageType::Serialization, "Serialization"},
    {MessageType::TestFarm, "TestFarm"}, {MessageType::ProductionTest, "ProductionTest"},
    {MessageType::LoadAutomatedTestPreset, "LoadAutomatedTestPreset"},
    {MessageType::SetTestPresetInputOutputPorts, "SetTestPresetInputOutputPorts"},
    {MessageType::SetTestPresetSplitMixPoints, "SetTestPresetSplitMixPoints"},
    {MessageType::GenerateTestPreset, "GenerateTestPreset"},
    {MessageType::ModelPreset, "ModelPreset"}, {MessageType::RemoteControl, "RemoteControl"},
    {MessageType::NumberOfMessageTypes, "NumberOfMessageTypes"},
}};

constexpr std::array<std::string_view, 9> kActions{"CREATE", "UPDATE", "DELETE", "READ", "MOVE",
                                                    "COPY",   "UPLOAD", "DOWNLOAD", "SWAP"};

}  // namespace

std::string_view to_string(MessageType t) noexcept {
    for (const auto& [type, name] : kNames) {
        if (type == t) return name;
    }
    return "Unknown";
}

std::string_view to_string(Action a) noexcept {
    const auto i = static_cast<std::size_t>(a);
    return i < kActions.size() ? kActions[i] : std::string_view{"Unknown"};
}

std::optional<MessageType> parse_message_type(std::string_view name) noexcept {
    for (const auto& [type, n] : kNames) {
        if (n == name) return type;
    }
    // Auch die blosse Nummer annehmen. Beim Erkunden kennt man den Namen oft
    // noch nicht - so liess sich Nachricht 71 ueberhaupt erst ansprechen.
    if (!name.empty() && std::all_of(name.begin(), name.end(),
                                     [](char c) { return c >= '0' && c <= '9'; })) {
        unsigned long v = 0;
        for (char c : name) {
            v = v * 10 + static_cast<unsigned long>(c - '0');
            if (v > raw(MessageType::NumberOfMessageTypes)) return std::nullopt;
        }
        return static_cast<MessageType>(v);
    }
    return std::nullopt;
}

MessageType message_type_from_raw(std::uint16_t v) noexcept {
    return v <= raw(MessageType::NumberOfMessageTypes) ? static_cast<MessageType>(v) : MessageType::Undefined;
}

}  // namespace qc
