#include "config/ConfigManager.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace ryugba::config {

namespace {

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::size_t findJsonKey(std::string_view json, std::string_view key, std::size_t startPos = 0) {
    const std::string needle = "\"" + std::string(key) + "\"";
    std::size_t pos = json.find(needle, startPos);
    while (pos != std::string_view::npos) {
        std::size_t after = pos + needle.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t' || json[after] == '\r' || json[after] == '\n')) {
            ++after;
        }
        if (after < json.size() && json[after] == ':') {
            return after + 1;
        }
        pos = json.find(needle, pos + 1);
    }
    return std::string_view::npos;
}

void skipWs(std::string_view s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
        ++pos;
    }
}

bool parseStringValue(std::string_view s, std::size_t& pos, std::string& out) {
    skipWs(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\' && pos < s.size()) {
            const char esc = s[pos++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                default:   out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

std::string readStringField(std::string_view section, std::string_view key, std::string_view fallback) {
    std::size_t pos = findJsonKey(section, key);
    if (pos == std::string_view::npos) return std::string(fallback);
    std::string out;
    if (parseStringValue(section, pos, out)) return out;
    return std::string(fallback);
}

int readIntField(std::string_view section, std::string_view key, int fallback) {
    std::size_t pos = findJsonKey(section, key);
    if (pos == std::string_view::npos) return fallback;
    skipWs(section, pos);
    std::size_t end = pos;
    if (end < section.size() && (section[end] == '-' || section[end] == '+')) ++end;
    while (end < section.size() && section[end] >= '0' && section[end] <= '9') ++end;
    if (end == pos) return fallback;
    try {
        return std::stoi(std::string(section.substr(pos, end - pos)));
    } catch (...) {
        return fallback;
    }
}

float readFloatField(std::string_view section, std::string_view key, float fallback) {
    std::size_t pos = findJsonKey(section, key);
    if (pos == std::string_view::npos) return fallback;
    skipWs(section, pos);
    std::size_t end = pos;
    while (end < section.size() &&
           ((section[end] >= '0' && section[end] <= '9') || section[end] == '.' || section[end] == '-' || section[end] == '+')) {
        ++end;
    }
    if (end == pos) return fallback;
    try {
        return std::stof(std::string(section.substr(pos, end - pos)));
    } catch (...) {
        return fallback;
    }
}

bool readBoolField(std::string_view section, std::string_view key, bool fallback) {
    std::size_t pos = findJsonKey(section, key);
    if (pos == std::string_view::npos) return fallback;
    skipWs(section, pos);
    if (section.substr(pos).starts_with("true"))  return true;
    if (section.substr(pos).starts_with("false")) return false;
    return fallback;
}

std::string_view extractObjectBlock(std::string_view json, std::string_view key) {
    std::size_t pos = findJsonKey(json, key);
    if (pos == std::string_view::npos) return {};
    skipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return {};
    const std::size_t start = pos;
    int depth = 0;
    bool inStr = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (c == '"' && (pos == 0 || json[pos - 1] != '\\')) inStr = !inStr;
        if (inStr) continue;
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
    }
    return {};
}

DualInputSlot readSlot(std::string_view parentObj, std::string_view slotName, const DualInputSlot& fallback) {
    const std::string_view obj = extractObjectBlock(parentObj, slotName);
    if (obj.empty()) return fallback;
    DualInputSlot slot = fallback;
    slot.keyScancode = readIntField(obj, "key", fallback.keyScancode);
    slot.joyType     = static_cast<JoyBindType>(std::clamp(readIntField(obj, "joyType", static_cast<int>(fallback.joyType)), 0, 5));
    slot.joyCode     = readIntField(obj, "joyCode", fallback.joyCode);
    return slot;
}

} // namespace

ConfigManager::ConfigManager()
    : m_config(makeDefaults())
    , m_configPath(defaultConfigPath()) {}

std::filesystem::path ConfigManager::defaultConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "ryugba" / "config.json";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "ryugba" / "config.json";
    }
    return std::filesystem::path("config.json");
}

AppConfig ConfigManager::makeDefaults() {
    AppConfig cfg{};

    // Controles padrão híbridos NDS / GBA (12 Botões: Teclado + SDL GameController estilo melonDS)
    cfg.controls[static_cast<std::size_t>(GbaAction::A)]      = {SDL_SCANCODE_X,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_A};
    cfg.controls[static_cast<std::size_t>(GbaAction::B)]      = {SDL_SCANCODE_Z,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_B};
    cfg.controls[static_cast<std::size_t>(GbaAction::X)]      = {SDL_SCANCODE_S,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_X};
    cfg.controls[static_cast<std::size_t>(GbaAction::Y)]      = {SDL_SCANCODE_A,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_Y};
    cfg.controls[static_cast<std::size_t>(GbaAction::L)]      = {SDL_SCANCODE_Q,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
    cfg.controls[static_cast<std::size_t>(GbaAction::R)]      = {SDL_SCANCODE_E,         JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
    cfg.controls[static_cast<std::size_t>(GbaAction::Start)]  = {SDL_SCANCODE_RETURN,    JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_START};
    cfg.controls[static_cast<std::size_t>(GbaAction::Select)] = {SDL_SCANCODE_BACKSPACE, JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_BACK};
    cfg.controls[static_cast<std::size_t>(GbaAction::Up)]     = {SDL_SCANCODE_UP,        JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_DPAD_UP};
    cfg.controls[static_cast<std::size_t>(GbaAction::Down)]   = {SDL_SCANCODE_DOWN,      JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_DPAD_DOWN};
    cfg.controls[static_cast<std::size_t>(GbaAction::Left)]   = {SDL_SCANCODE_LEFT,      JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
    cfg.controls[static_cast<std::size_t>(GbaAction::Right)]  = {SDL_SCANCODE_RIGHT,     JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};

    // Hotkeys & Sensores Especiais DS estilo melonDS
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::FastForward)] = {SDL_SCANCODE_TAB, JoyBindType::ControllerAxisPos, SDL_CONTROLLER_AXIS_TRIGGERRIGHT};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::QuickSave)]   = {SDL_SCANCODE_F5,  JoyBindType::None, 0};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::QuickLoad)]   = {SDL_SCANCODE_F8,  JoyBindType::None, 0};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::Pause)]       = {SDL_SCANCODE_P,   JoyBindType::None, 0};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::MicBlow)]     = {SDL_SCANCODE_M,   JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_LEFTSTICK};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::SwapScreens)] = {SDL_SCANCODE_F6,  JoyBindType::ControllerButton, SDL_CONTROLLER_BUTTON_RIGHTSTICK};
    cfg.hotkeys[static_cast<std::size_t>(HotkeyAction::ToggleLid)]   = {SDL_SCANCODE_F7,  JoyBindType::None, 0};

    return cfg;
}

void ConfigManager::resetControlsToDefault() {
    const AppConfig def = makeDefaults();
    m_config.controls = def.controls;
    m_config.hotkeys  = def.hotkeys;
}

bool ConfigManager::load() {
    return loadFrom(m_configPath);
}

bool ConfigManager::loadFrom(const std::filesystem::path& path) {
    m_configPath = path;
    std::ifstream in(path);
    if (!in) return false;

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    if (json.empty()) return false;

    AppConfig parsed = makeDefaults();
    parsed.speedPercent = readIntField(json, "speedPercent", 100);

    if (std::size_t dirPos = findJsonKey(json, "lastBrowseDirectory"); dirPos != std::string_view::npos) {
        parseStringValue(json, dirPos, parsed.lastBrowseDirectory);
    }

    if (const auto vid = extractObjectBlock(json, "video"); !vid.empty()) {
        parsed.video.scale             = std::clamp(readIntField(vid, "scale", 3), 1, 4);
        parsed.video.aspectRatio       = static_cast<AspectRatioMode>(std::clamp(readIntField(vid, "aspectRatio", 0), 0, 2));
        parsed.video.ndsLayout         = static_cast<NdsScreenLayout>(std::clamp(readIntField(vid, "ndsLayout", 0), 0, 2));
        parsed.video.swapScreens       = readBoolField(vid, "swapScreens", false);
        parsed.video.showStylusCursor  = readBoolField(vid, "showStylusCursor", true);
        parsed.video.bilinearFilter    = readBoolField(vid, "bilinearFilter", false);
        parsed.video.frameLimiter60Fps = readBoolField(vid, "frameLimiter60Fps", true);
    }

    if (const auto aud = extractObjectBlock(json, "audio"); !aud.empty()) {
        parsed.audio.masterVolume  = std::clamp(readFloatField(aud, "masterVolume", 0.80f), 0.0f, 1.0f);
        parsed.audio.muted         = readBoolField(aud, "muted", false);
        parsed.audio.bufferSamples = readIntField(aud, "bufferSamples", 1024);
        if (parsed.audio.bufferSamples != 512 && parsed.audio.bufferSamples != 1024 && parsed.audio.bufferSamples != 2048) {
            parsed.audio.bufferSamples = 1024;
        }
        parsed.audio.sampleRate = readIntField(aud, "sampleRate", 48000);
        if (parsed.audio.sampleRate != 32768 && parsed.audio.sampleRate != 44100 && parsed.audio.sampleRate != 48000) {
            parsed.audio.sampleRate = 48000;
        }
    }

    if (const auto bootObj = extractObjectBlock(json, "ndsBoot"); !bootObj.empty()) {
        parsed.ndsBoot.directBootHle = readBoolField(bootObj, "directBootHle", true);
        parsed.ndsBoot.bios9Path     = readStringField(bootObj, "bios9Path", "bios9.bin");
        parsed.ndsBoot.bios7Path     = readStringField(bootObj, "bios7Path", "bios7.bin");
        parsed.ndsBoot.firmwarePath  = readStringField(bootObj, "firmwarePath", "firmware.bin");
    }

    if (const auto ctrlObj = extractObjectBlock(json, "controls"); !ctrlObj.empty()) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(GbaAction::Count); ++i) {
            const auto act = static_cast<GbaAction>(i);
            parsed.controls[i] = readSlot(ctrlObj, gbaActionJsonKey(act), parsed.controls[i]);
        }
    }

    if (const auto hkObj = extractObjectBlock(json, "hotkeys"); !hkObj.empty()) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(HotkeyAction::Count); ++i) {
            const auto hk = static_cast<HotkeyAction>(i);
            parsed.hotkeys[i] = readSlot(hkObj, hotkeyActionJsonKey(hk), parsed.hotkeys[i]);
        }
    }

    if (std::size_t recPos = findJsonKey(json, "recentRoms"); recPos != std::string_view::npos) {
        skipWs(json, recPos);
        if (recPos < json.size() && json[recPos] == '[') {
            ++recPos;
            while (recPos < json.size()) {
                skipWs(json, recPos);
                if (recPos >= json.size() || json[recPos] == ']') break;
                if (json[recPos] == ',') { ++recPos; continue; }
                std::string entry;
                if (!parseStringValue(json, recPos, entry)) break;
                if (!entry.empty()) parsed.recentRoms.push_back(entry);
            }
        }
    }

    m_config = std::move(parsed);
    return true;
}

bool ConfigManager::save() const {
    return saveTo(m_configPath);
}

bool ConfigManager::saveTo(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "{\n"
        << "  \"emulator\": \"RyuDS_RyuGBA\",\n"
        << "  \"speedPercent\": " << m_config.speedPercent << ",\n"
        << "  \"lastBrowseDirectory\": \"" << jsonEscape(m_config.lastBrowseDirectory) << "\",\n"
        << "  \"video\": {\n"
        << "    \"scale\": " << m_config.video.scale << ",\n"
        << "    \"aspectRatio\": " << static_cast<int>(m_config.video.aspectRatio) << ",\n"
        << "    \"ndsLayout\": " << static_cast<int>(m_config.video.ndsLayout) << ",\n"
        << "    \"swapScreens\": " << (m_config.video.swapScreens ? "true" : "false") << ",\n"
        << "    \"showStylusCursor\": " << (m_config.video.showStylusCursor ? "true" : "false") << ",\n"
        << "    \"bilinearFilter\": " << (m_config.video.bilinearFilter ? "true" : "false") << ",\n"
        << "    \"frameLimiter60Fps\": " << (m_config.video.frameLimiter60Fps ? "true" : "false") << "\n"
        << "  },\n"
        << "  \"audio\": {\n"
        << "    \"masterVolume\": " << std::fixed << std::setprecision(2) << m_config.audio.masterVolume << ",\n"
        << "    \"muted\": " << (m_config.audio.muted ? "true" : "false") << ",\n"
        << "    \"bufferSamples\": " << m_config.audio.bufferSamples << ",\n"
        << "    \"sampleRate\": " << m_config.audio.sampleRate << "\n"
        << "  },\n"
        << "  \"ndsBoot\": {\n"
        << "    \"directBootHle\": " << (m_config.ndsBoot.directBootHle ? "true" : "false") << ",\n"
        << "    \"bios9Path\": \"" << jsonEscape(m_config.ndsBoot.bios9Path) << "\",\n"
        << "    \"bios7Path\": \"" << jsonEscape(m_config.ndsBoot.bios7Path) << "\",\n"
        << "    \"firmwarePath\": \"" << jsonEscape(m_config.ndsBoot.firmwarePath) << "\"\n"
        << "  },\n"
        << "  \"controls\": {\n";

    for (std::size_t i = 0; i < static_cast<std::size_t>(GbaAction::Count); ++i) {
        const auto act = static_cast<GbaAction>(i);
        const auto& s  = m_config.controls[i];
        out << "    \"" << gbaActionJsonKey(act) << "\": { \"key\": " << s.keyScancode
            << ", \"joyType\": " << static_cast<int>(s.joyType)
            << ", \"joyCode\": " << s.joyCode << " }"
            << (i + 1 < static_cast<std::size_t>(GbaAction::Count) ? "," : "") << "\n";
    }

    out << "  },\n"
        << "  \"hotkeys\": {\n";

    for (std::size_t i = 0; i < static_cast<std::size_t>(HotkeyAction::Count); ++i) {
        const auto hk = static_cast<HotkeyAction>(i);
        const auto& s = m_config.hotkeys[i];
        out << "    \"" << hotkeyActionJsonKey(hk) << "\": { \"key\": " << s.keyScancode
            << ", \"joyType\": " << static_cast<int>(s.joyType)
            << ", \"joyCode\": " << s.joyCode << " }"
            << (i + 1 < static_cast<std::size_t>(HotkeyAction::Count) ? "," : "") << "\n";
    }

    out << "  },\n"
        << "  \"recentRoms\": [\n";
    for (std::size_t i = 0; i < m_config.recentRoms.size(); ++i) {
        out << "    \"" << jsonEscape(m_config.recentRoms[i]) << "\""
            << (i + 1 < m_config.recentRoms.size() ? "," : "") << "\n";
    }
    out << "  ]\n"
        << "}\n";

    return out.good();
}

void ConfigManager::addRecentRom(const std::filesystem::path& romPath) {
    if (romPath.empty()) return;
    std::error_code ec;
    const auto canon = std::filesystem::weakly_canonical(romPath, ec);
    const std::string norm = ec ? romPath.string() : canon.string();

    auto& list = m_config.recentRoms;
    list.erase(std::remove(list.begin(), list.end(), norm), list.end());
    list.insert(list.begin(), norm);
    if (list.size() > 10) {
        list.resize(10);
    }
    if (romPath.has_parent_path()) {
        m_config.lastBrowseDirectory = romPath.parent_path().string();
    }
}

void ConfigManager::clearRecentRoms() {
    m_config.recentRoms.clear();
}

} // namespace ryugba::config
