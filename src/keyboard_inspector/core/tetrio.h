#pragma once

#include <QJsonDocument>
#include <QJsonObject>

#include <optional>
#include <string>
#include <vector>

#include "model.h"

namespace keyboard_inspector {

// Upstream TetrioReplay.StreamToResult, ported. TETR.IO downsamples inputs
// onto the 600 Hz subframe grid; the analysis is forced to BinRate 600.
class TetrioError : public std::exception {
public:
    explicit TetrioError(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

// One selectable entry in the multiplayer round picker: analyze this round's
// replay for the player identified by player_id.
struct TetrioRoundEntry {
    int round_index = 0;
    std::string player_id;
    std::string label;   // "1.23 PPS · 4.56 APM · 7.89 VS"
    std::string detail;  // "12 inputs · 1:03 · winner: NAME"
};

struct TetrioMultiplayer {
    std::string matchup;  // "P1 (wins) versus (wins) P2"
    std::vector<TetrioRoundEntry> rounds;
};

bool tetrio_is_multiplayer(const QJsonDocument& doc);

// Build the picker entries for a multiplayer (.ttrm) replay.
TetrioMultiplayer tetrio_inspect_multiplayer(const QJsonDocument& doc);

// Build a Recording from a solo replay, or — when round_index/player_id are
// given — from the selected player's replay inside a multiplayer round.
Recording tetrio_build_recording(const QJsonDocument& doc,
                                 std::optional<int> round_index = std::nullopt,
                                 const std::string& player_id = "");

} // namespace keyboard_inspector
