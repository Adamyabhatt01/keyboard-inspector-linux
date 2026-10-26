#include "keynames.h"

#include <unordered_map>
#include <linux/input.h>
#include <libevdev-1.0/libevdev/libevdev.h>

namespace keyboard_inspector {

namespace {

const std::unordered_map<int, std::string>& get_key_map() {
    static const std::unordered_map<int, std::string> map = {
        // Letters
        {KEY_A, "A"}, {KEY_B, "B"}, {KEY_C, "C"}, {KEY_D, "D"}, {KEY_E, "E"},
        {KEY_F, "F"}, {KEY_G, "G"}, {KEY_H, "H"}, {KEY_I, "I"}, {KEY_J, "J"},
        {KEY_K, "K"}, {KEY_L, "L"}, {KEY_M, "M"}, {KEY_N, "N"}, {KEY_O, "O"},
        {KEY_P, "P"}, {KEY_Q, "Q"}, {KEY_R, "R"}, {KEY_S, "S"}, {KEY_T, "T"},
        {KEY_U, "U"}, {KEY_V, "V"}, {KEY_W, "W"}, {KEY_X, "X"}, {KEY_Y, "Y"},
        {KEY_Z, "Z"},

        // Digits
        {KEY_1, "D1"}, {KEY_2, "D2"}, {KEY_3, "D3"}, {KEY_4, "D4"}, {KEY_5, "D5"},
        {KEY_6, "D6"}, {KEY_7, "D7"}, {KEY_8, "D8"}, {KEY_9, "D9"}, {KEY_0, "D0"},

        // Function keys
        {KEY_F1, "F1"},   {KEY_F2, "F2"},   {KEY_F3, "F3"},   {KEY_F4, "F4"},
        {KEY_F5, "F5"},   {KEY_F6, "F6"},   {KEY_F7, "F7"},   {KEY_F8, "F8"},
        {KEY_F9, "F9"},   {KEY_F10, "F10"}, {KEY_F11, "F11"}, {KEY_F12, "F12"},
        {KEY_F13, "F13"}, {KEY_F14, "F14"}, {KEY_F15, "F15"}, {KEY_F16, "F16"},
        {KEY_F17, "F17"}, {KEY_F18, "F18"}, {KEY_F19, "F19"}, {KEY_F20, "F20"},
        {KEY_F21, "F21"}, {KEY_F22, "F22"}, {KEY_F23, "F23"}, {KEY_F24, "F24"},

        // Keypad
        {KEY_KP0, "NumPad0"}, {KEY_KP1, "NumPad1"}, {KEY_KP2, "NumPad2"},
        {KEY_KP3, "NumPad3"}, {KEY_KP4, "NumPad4"}, {KEY_KP5, "NumPad5"},
        {KEY_KP6, "NumPad6"}, {KEY_KP7, "NumPad7"}, {KEY_KP8, "NumPad8"},
        {KEY_KP9, "NumPad9"},
        {KEY_KPENTER, "Return"},
        {KEY_KPSLASH, "Divide"},
        {KEY_KPASTERISK, "Multiply"},
        {KEY_KPMINUS, "Subtract"},
        {KEY_KPPLUS, "Add"},
        {KEY_KPDOT, "Decimal"},
        {KEY_KPEQUAL, "Oemplus"},

        // Navigation and editing
        {KEY_ESC, "Escape"},
        {KEY_TAB, "Tab"},
        {KEY_CAPSLOCK, "Capital"},
        {KEY_LEFTSHIFT, "LShiftKey"},
        {KEY_RIGHTSHIFT, "RShiftKey"},
        {KEY_LEFTCTRL, "LControlKey"},
        {KEY_RIGHTCTRL, "RControlKey"},
        {KEY_LEFTALT, "LMenu"},
        {KEY_RIGHTALT, "RMenu"},
        {KEY_LEFTMETA, "LWin"},
        {KEY_RIGHTMETA, "RWin"},
        {KEY_SPACE, "Space"},
        {KEY_ENTER, "Return"},
        {KEY_BACKSPACE, "Back"},
        {KEY_INSERT, "Insert"},
        {KEY_DELETE, "Delete"},
        {KEY_HOME, "Home"},
        {KEY_END, "End"},
        {KEY_PAGEUP, "PageUp"},
        {KEY_PAGEDOWN, "PageDown"},
        {KEY_UP, "Up"},
        {KEY_DOWN, "Down"},
        {KEY_LEFT, "Left"},
        {KEY_RIGHT, "Right"},
        {KEY_MENU, "Apps"},
        {KEY_COMPOSE, "Apps"},
        {KEY_PAUSE, "Pause"},
        {KEY_SYSRQ, "Snapshot"},
        {KEY_SCROLLLOCK, "Scroll"},
        {KEY_NUMLOCK, "NumLock"},

        // OEM and symbols
        {KEY_102ND, "OemBackslash"},
        {KEY_MINUS, "OemMinus"},
        {KEY_EQUAL, "Oemplus"},
        {KEY_LEFTBRACE, "OemOpenBrackets"},
        {KEY_RIGHTBRACE, "OemCloseBrackets"},
        {KEY_BACKSLASH, "OemPipe"},
        {KEY_SEMICOLON, "OemSemicolon"},
        {KEY_APOSTROPHE, "OemQuotes"},
        {KEY_GRAVE, "Oemtilde"},
        {KEY_COMMA, "Oemcomma"},
        {KEY_DOT, "OemPeriod"},
        {KEY_SLASH, "OemQuestion"},

        // Media and browser
        {KEY_PLAYPAUSE, "MediaPlayPause"},
        {KEY_NEXTSONG, "MediaNextTrack"},
        {KEY_PREVIOUSSONG, "MediaPreviousTrack"},
        {KEY_STOPCD, "MediaStop"},
        {KEY_MUTE, "VolumeMute"},
        {KEY_VOLUMEDOWN, "VolumeDown"},
        {KEY_VOLUMEUP, "VolumeUp"},
        {KEY_BACK, "BrowserBack"},
        {KEY_FORWARD, "BrowserForward"},
        {KEY_REFRESH, "BrowserRefresh"},
        {KEY_STOP, "BrowserStop"},
        {KEY_SEARCH, "BrowserSearch"},
        {KEY_BOOKMARKS, "BrowserFavorites"},
        {KEY_HOMEPAGE, "BrowserHome"},
        {KEY_MAIL, "LaunchMail"},
        {KEY_MEDIA, "LaunchMediaSelect"},
        {KEY_CALC, "LaunchApplication1"},
        {KEY_POWER, "Power"},
        {KEY_SLEEP, "Sleep"},
        {KEY_WAKEUP, "Wake"},
        {KEY_HANGEUL, "HangulMode"},
        {KEY_HANJA, "HanjaMode"},
        {KEY_KATAKANA, "Katakana"},

        // Mouse buttons
        {BTN_LEFT, "LButton"},
        {BTN_RIGHT, "RButton"},
        {BTN_MIDDLE, "MButton"},
        {BTN_SIDE, "XButton1"},
        {BTN_EXTRA, "XButton2"},
    };
    return map;
}

const std::unordered_map<int, std::string>& get_mouse_map() {
    static const std::unordered_map<int, std::string> map = {
        {BTN_LEFT, "LeftClick"},
        {BTN_RIGHT, "RightClick"},
        {BTN_MIDDLE, "MiddleClick"},
        {BTN_SIDE, "MouseBack"},
        {BTN_BACK, "MouseBack"},
        {BTN_EXTRA, "MouseForward"},
        {BTN_FORWARD, "MouseForward"},
    };
    return map;
}

const std::unordered_map<int, std::string>& get_gamepad_map() {
    static const std::unordered_map<int, std::string> map = {
        {BTN_SOUTH, "Button1"},
        {BTN_EAST, "Button2"},
        {BTN_NORTH, "Button3"},
        {BTN_WEST, "Button4"},
        {BTN_TL, "Button5"},
        {BTN_TR, "Button6"},
        {BTN_TL2, "Button7"},
        {BTN_TR2, "Button8"},
        {BTN_SELECT, "Button9"},
        {BTN_START, "Button10"},
        {BTN_MODE, "Button11"},
        {BTN_THUMBL, "Button12"},
        {BTN_THUMBR, "Button13"},
        {BTN_C, "Button14"},
        {BTN_Z, "Button15"},
    };
    return map;
}

} // namespace

std::string key_label(int code) {
    const auto& map = get_key_map();
    auto it = map.find(code);
    if (it != map.end()) {
        return it->second;
    }

    const char* kernel_name = libevdev_event_code_get_name(EV_KEY, code);
    if (kernel_name && kernel_name[0] != '\0') {
        return kernel_name;
    }

    return "KEY_" + std::to_string(code);
}

std::string input_label(int code, const std::string& kind) {
    if (kind == "mouse") {
        const auto& mmap = get_mouse_map();
        auto it = mmap.find(code);
        if (it != mmap.end()) {
            return it->second;
        }
    } else if (kind == "gamepad") {
        const auto& gmap = get_gamepad_map();
        auto it = gmap.find(code);
        if (it != gmap.end()) {
            return it->second;
        }
    }
    return key_label(code);
}

// Names for the WinForms Keys enum values that are used to decode legacy v0
// .kbi files. Duplicate enum values resolve to the .NET Framework ToString()
// name for that value (Return, Capital, PageUp, PageDown, Snapshot, ...),
// matching what the Windows original displays for those keys. Undefined VK
// codes print as their decimal value, like FlagsEnum.ToString() does.
std::string windows_key_name(int vk) {
    static const std::string names[] = {
        "None", "LButton", "RButton", "Cancel", "MButton", "XButton1", "XButton2", "", "Back",
        "Tab", "LineFeed", "", "Clear", "Return", "", "", "ShiftKey", "ControlKey", "Menu",
        "Pause", "Capital", "HangulMode", "", "JunjaMode", "FinalMode", "HanjaMode", "", "Escape",
        "IMEConvert", "IMENonconvert", "IMEAccept", "IMEModeChange", "Space", "PageUp", "PageDown",
        "End", "Home", "Left", "Up", "Right", "Down", "Select", "Print", "Execute", "Snapshot",
        "Insert", "Delete", "Help",
        "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9",
    };
    if (vk >= 0 && vk <= 57) {
        return names[vk].empty() ? std::to_string(vk) : names[vk];
    }
    if (vk >= 65 && vk <= 90) {
        return std::string(1, static_cast<char>('A' + (vk - 65)));
    }
    if (vk >= 91 && vk <= 105) {
        static const char* win_apps[] = {"LWin", "RWin", "Apps", "", "Sleep",
                                         "NumPad0", "NumPad1", "NumPad2", "NumPad3", "NumPad4",
                                         "NumPad5", "NumPad6", "NumPad7", "NumPad8", "NumPad9"};
        return win_apps[vk - 91];
    }
    if (vk >= 106 && vk <= 111) {
        static const char* numpad_ops[] = {"Multiply", "Add", "Separator", "Subtract", "Decimal", "Divide"};
        return numpad_ops[vk - 106];
    }
    if (vk >= 112 && vk <= 135) {
        return "F" + std::to_string(vk - 111);
    }
    if (vk == 144) return "NumLock";
    if (vk == 145) return "Scroll";
    if (vk >= 160 && vk <= 165) {
        static const char* mods[] = {"LShiftKey", "RShiftKey", "LControlKey", "RControlKey", "LMenu", "RMenu"};
        return mods[vk - 160];
    }
    if (vk >= 166 && vk <= 172) {
        static const char* browser[] = {"BrowserBack", "BrowserForward", "BrowserRefresh", "BrowserStop",
                                        "BrowserSearch", "BrowserFavorites", "BrowserHome"};
        return browser[vk - 166];
    }
    if (vk >= 173 && vk <= 175) {
        static const char* volume[] = {"VolumeMute", "VolumeDown", "VolumeUp"};
        return volume[vk - 173];
    }
    if (vk >= 176 && vk <= 183) {
        static const char* media[] = {"MediaNextTrack", "MediaPreviousTrack", "MediaStop", "MediaPlayPause",
                                      "LaunchMail", "SelectMedia", "LaunchApplication1", "LaunchApplication2"};
        return media[vk - 176];
    }
    if (vk >= 186 && vk <= 192) {
        static const char* oem[] = {"OemSemicolon", "Oemplus", "Oemcomma", "OemMinus", "OemPeriod",
                                    "OemQuestion", "Oemtilde"};
        return oem[vk - 186];
    }
    if (vk >= 219 && vk <= 223) {
        static const char* oem_bracket[] = {"OemOpenBrackets", "OemPipe", "OemCloseBrackets", "OemQuotes", "Oem8"};
        return oem_bracket[vk - 219];
    }
    if (vk == 226) return "OemBackslash";
    if (vk == 229) return "ProcessKey";
    if (vk == 231) return "Packet";
    if (vk == 246) return "Attn";
    if (vk == 247) return "Crsel";
    if (vk == 248) return "Exsel";
    if (vk == 249) return "EraseEof";
    if (vk == 250) return "Play";
    if (vk == 251) return "Zoom";
    if (vk == 252) return "NoName";
    if (vk == 253) return "Pa1";
    if (vk == 254) return "OemClear";
    return std::to_string(vk);
}

std::string gamepad_key_name(int index) {
    if (index >= 0 && index <= 31) return "Button" + std::to_string(index + 1);
    static const char* hats[] = {"HatN", "HatE", "HatS", "HatW"};
    if (index >= 32 && index <= 35) return hats[index - 32];
    return std::to_string(index);
}

std::string mouse_key_name(int index) {
    static const char* names[] = {"LeftClick", "RightClick", "MiddleClick", "MouseBack", "MouseForward"};
    if (index >= 0 && index <= 4) return names[index];
    return std::to_string(index);
}

std::string tetrio_key_name(int index) {
    static const char* names[] = {"moveLeft", "moveRight", "softDrop", "hardDrop",
                                  "rotateCCW", "rotateCW", "rotate180", "hold"};
    if (index >= 0 && index <= 7) return names[index];
    return std::to_string(index);
}

} // namespace keyboard_inspector
