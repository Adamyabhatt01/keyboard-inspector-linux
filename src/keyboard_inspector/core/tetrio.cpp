#include "tetrio.h"

#include <QDateTime>
#include <QJsonArray>

#include <cmath>
#include <cstdio>
#include <ctime>

namespace keyboard_inspector {

namespace {

// Upstream TetrioReplay.gamemodes
const char* gamemode_title(const std::string& gamemode) {
    if (gamemode == "league") return "TETRA LEAGUE";
    if (gamemode == "40l") return "40 LINES";
    if (gamemode == "blitz") return "BLITZ";
    if (gamemode == "zenith") return "QUICK PLAY";
    if (gamemode == "zenithex") return "EXPERT QUICK PLAY";
    return nullptr;
}

// Format like C# "{value:#,##0}" (thousands separators, no decimals).
std::string format_grouped(double value) {
    long long rounded = static_cast<long long>(std::llround(value));
    char raw[32];
    snprintf(raw, sizeof(raw), "%lld", rounded);
    std::string digits(raw);
    std::string out;
    int count = 0;
    for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
        out.insert(out.begin(), digits[i]);
        if (++count % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out;
}

std::string title_timestamp(const QDateTime& local) {
    std::tm tm{};
    time_t t = local.toSecsSinceEpoch();
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%m/%d/%Y, %I:%M:%S %p", &tm);
    return buf;
}

std::chrono::system_clock::time_point to_time_point(const QDateTime& local) {
    return std::chrono::system_clock::from_time_t(local.toSecsSinceEpoch());
}

QDateTime parse_timestamp(const QJsonObject& root) {
    QDateTime ts = QDateTime::fromString(root.value("ts").toString(), Qt::ISODateWithMs);
    if (!ts.isValid()) {
        ts = QDateTime::fromString(root.value("ts").toString(), Qt::ISODate);
    }
    if (!ts.isValid()) {
        ts = QDateTime::currentDateTimeUtc();
    }
    return ts.toLocalTime();
}

std::vector<Event> parse_events(const QJsonObject& replay) {
    std::vector<Event> events;
    QJsonArray raw = replay.value("events").toArray();
    events.reserve(static_cast<size_t>(raw.size()));
    for (const auto& item : raw) {
        QJsonObject entry = item.toObject();
        std::string type = entry.value("type").toString().toStdString();
        if (type.rfind("key", 0) != 0) continue;
        QJsonObject data = entry.value("data").toObject();
        if (data.value("hoisted").toBool()) continue;

        double frame = entry.value("frame").toDouble();
        double subframe = data.value("subframe").toDouble();
        double time = std::round(frame + subframe) / 60.0;
        InputId input_id{0, data.value("key").toString().toStdString()};
        events.push_back(Event{time, type == "keydown", input_id});
    }
    return events;
}

SourceInfo tetrio_source(int64_t count) {
    return SourceInfo{0, "TETR.IO Replay", "", count};
}

} // namespace

bool tetrio_is_multiplayer(const QJsonDocument& doc) {
    return doc.object().value("users").toArray().size() > 1;
}

TetrioMultiplayer tetrio_inspect_multiplayer(const QJsonDocument& doc) {
    QJsonObject root = doc.object();
    if (root.value("version").toInt(-1) < 1) {
        throw TetrioError("Only TETR.IO Beta replays are supported.");
    }

    QJsonObject replay = root.value("replay").toObject();
    QJsonArray leaderboard = replay.value("leaderboard").toArray();
    if (leaderboard.size() < 2) throw TetrioError("Replay leaderboard is malformed.");
    QJsonArray rounds = replay.value("rounds").toArray();

    std::string p1_id = leaderboard[0].toObject().value("id").toString().toStdString();
    std::string p2_id = leaderboard[1].toObject().value("id").toString().toStdString();

    auto stats_of = [](const QJsonObject& entry) {
        QJsonObject stats = entry.value("stats").toObject();
        return std::tuple<double, double, double>(
            stats.value("pps").toDouble(),
            stats.value("apm").toDouble(),
            stats.value("vsscore").toDouble());
    };

    TetrioMultiplayer info;
    auto [p1_pps, p1_apm, p1_vs] = stats_of(leaderboard[0].toObject());
    auto [p2_pps, p2_apm, p2_vs] = stats_of(leaderboard[1].toObject());
    info.matchup =
        leaderboard[0].toObject().value("username").toString().toUpper().toStdString() +
        " (" + std::to_string(leaderboard[0].toObject().value("wins").toInt()) + ") versus (" +
        std::to_string(leaderboard[1].toObject().value("wins").toInt()) + ") " +
        leaderboard[1].toObject().value("username").toString().toUpper().toStdString();

    auto stat_text = [](double pps, double apm, double vs) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%.2f PPS · %.2f APM · %.2f VS", pps, apm, vs);
        return std::string(buf);
    };

    int64_t total_inputs1 = 0;
    int64_t total_inputs2 = 0;
    auto count_inputs = [](const QJsonObject& player_entry) {
        int64_t count = 0;
        QJsonArray replay_events = player_entry.value("replay").toObject().value("events").toArray();
        for (const auto& item : replay_events) {
            QJsonObject entry = item.toObject();
            std::string type = entry.value("type").toString().toStdString();
            if (type.rfind("key", 0) != 0) continue;
            if (entry.value("data").toObject().value("hoisted").toBool()) continue;
            ++count;
        }
        return count;
    };
    total_inputs1 = count_inputs(leaderboard[0].toObject());
    total_inputs2 = count_inputs(leaderboard[1].toObject());

    char p1_summary[96];
    snprintf(p1_summary, sizeof(p1_summary), "%s · %lld inputs",
             stat_text(p1_pps, p1_apm, p1_vs).c_str(), static_cast<long long>(total_inputs1));
    char p2_summary[96];
    snprintf(p2_summary, sizeof(p2_summary), "%s · %lld inputs",
             stat_text(p2_pps, p2_apm, p2_vs).c_str(), static_cast<long long>(total_inputs2));

    for (int i = 0; i < rounds.size(); ++i) {
        QJsonArray round = rounds[i].toArray();
        if (round.size() < 2) continue;
        QJsonObject entry1 = round[0].toObject();
        QJsonObject entry2 = round[1].toObject();

        // Upstream picks the entry whose id matches the leaderboard player.
        QJsonObject side1 = entry1.value("id").toString() == QString::fromStdString(p1_id)
                                ? entry1 : entry2;
        QJsonObject side2 = entry2.value("id").toString() == QString::fromStdString(p2_id)
                                ? entry2 : entry1;

        auto [s1_pps, s1_apm, s1_vs] = stats_of(side1);
        auto [s2_pps, s2_apm, s2_vs] = stats_of(side2);

        double frames = std::min(side1.value("replay").toObject().value("frames").toDouble(),
                                 side2.value("replay").toObject().value("frames").toDouble());
        int seconds = static_cast<int>(frames / 60.0);
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", seconds / 60, seconds % 60);

        std::string winner = side1.value("alive").toBool()
            ? side1.value("id").toString().toStdString()
            : side2.value("id").toString().toStdString();

        TetrioRoundEntry left;
        left.round_index = i;
        left.player_id = p1_id;
        left.label = stat_text(s1_pps, s1_apm, s1_vs);
        left.detail = std::string("Round ") + std::to_string(i + 1) + "/" +
                      std::to_string(rounds.size()) + " · " + time_buf + " · " + p1_summary +
                      (winner == p1_id ? " · P1 wins" : " · P2 wins");
        info.rounds.push_back(std::move(left));

        TetrioRoundEntry right;
        right.round_index = i;
        right.player_id = p2_id;
        right.label = stat_text(s2_pps, s2_apm, s2_vs);
        right.detail = std::string("Round ") + std::to_string(i + 1) + "/" +
                       std::to_string(rounds.size()) + " · " + time_buf + " · " + p2_summary +
                       (winner == p2_id ? " · P2 wins" : " · P1 wins");
        info.rounds.push_back(std::move(right));
    }

    return info;
}

Recording tetrio_build_recording(const QJsonDocument& doc,
                                 std::optional<int> round_index,
                                 const std::string& player_id) {
    QJsonObject root = doc.object();
    if (root.value("version").toInt(-1) < 1) {
        throw TetrioError("Only TETR.IO Beta replays are supported.");
    }

    std::string title;
    std::string gamemode = root.value("gamemode").toString().toStdString();
    if (const char* mode = gamemode_title(gamemode)) {
        title += std::string("[") + mode + "] ";
    }

    bool is_multi = tetrio_is_multiplayer(doc);
    QDateTime ts = parse_timestamp(root);
    QJsonObject data;

    if (is_multi) {
        if (!round_index) throw TetrioError("Multiplayer replays need a round selection.");

        QJsonObject replay = root.value("replay").toObject();
        QJsonArray leaderboard = replay.value("leaderboard").toArray();
        QJsonArray rounds = replay.value("rounds").toArray();
        if (*round_index < 0 || *round_index >= rounds.size()) {
            throw TetrioError("Replay round selection is out of range.");
        }

        QJsonArray round = rounds[*round_index].toArray();
        QJsonObject chosen;
        for (const auto& item : round) {
            QJsonObject entry = item.toObject();
            if (entry.value("id").toString() == QString::fromStdString(player_id)) {
                chosen = entry.value("replay").toObject();
                break;
            }
        }
        if (chosen.isEmpty()) throw TetrioError("Replay round selection is malformed.");
        data = chosen;

        int p1 = leaderboard[0].toObject().value("id").toString() == QString::fromStdString(player_id) ? 0 : 1;
        int p2 = 1 - p1;
        title += leaderboard[p1].toObject().value("username").toString().toUpper().toStdString() +
                 " (" + std::to_string(leaderboard[p1].toObject().value("wins").toInt()) + ") versus (" +
                 std::to_string(leaderboard[p2].toObject().value("wins").toInt()) + ") " +
                 leaderboard[p2].toObject().value("username").toString().toUpper().toStdString() +
                 ", round " + std::to_string(*round_index + 1) + "/" +
                 std::to_string(rounds.size()) + ", played ";
    } else {
        data = root.value("replay").toObject();
        if (gamemode == "40l") {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.3f ",
                     data.value("results").toObject().value("stats").toObject().value("finaltime").toDouble() / 1000.0);
            title += buf;
        }
        if (gamemode == "blitz") {
            title += format_grouped(
                data.value("results").toObject().value("stats").toObject().value("score").toDouble()) + " ";
        }
        if (gamemode == "zenith" || gamemode == "zenithex") {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f m ",
                     data.value("results").toObject().value("stats").toObject().value("zenith").toObject().value("altitude").toDouble());
            title += buf;
        }

        std::string user = root.value("users").toArray()[0].toObject().value("username").toString().toStdString();
        if (!user.empty() && user.find_first_not_of(" \t\n\r") != std::string::npos) {
            title += "played by " + user + " ";
        }
    }

    title += "on " + title_timestamp(ts);

    std::vector<Event> events = parse_events(data);
    if (events.empty()) {
        throw TetrioError("The replay contains no key events.");
    }

    std::map<int64_t, SourceInfo> sources;
    sources[0] = tetrio_source(static_cast<int64_t>(events.size()));

    auto groups = group_input_events(events);
    std::vector<InputInfo> inputs;
    inputs.reserve(groups.size());
    for (const auto& [input_id, group] : groups) {
        inputs.push_back(InputInfo{input_id, DEFAULT_INPUT_COLOR, true, group});
    }

    Recording rec;
    rec.title = title;
    rec.recorded = to_time_point(ts);
    rec.duration = data.value("frames").toDouble() / 60.0;
    rec.events = std::move(events);
    rec.sources = std::move(sources);
    rec.inputs = std::move(inputs);
    // Upstream forces BinRate 600 for TETR.IO replays (subframe grid).
    rec.settings = AnalysisSettings{600, 0, true};
    rec.measurement_kind = MeasurementKind::TETRIO;
    return rec;
}

} // namespace keyboard_inspector
