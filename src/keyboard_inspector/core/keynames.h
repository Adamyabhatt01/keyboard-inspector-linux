#pragma once

#include <string>

namespace keyboard_inspector {

std::string key_label(int code);
std::string input_label(int code, const std::string& kind = "keyboard");

// .NET Framework Keys enum name for a virtual-key code 0..255, used to decode
// legacy .kbi v0 files. Undefined VKs return the decimal number, matching
// FlagsEnum.ToString() behavior for undefined values.
std::string windows_key_name(int vk);

// Legacy v0 GamepadKeys (Button1..Button32, HatN=32..HatW=35), MouseKeys and
// TetrioKeys display names.
std::string gamepad_key_name(int index);
std::string mouse_key_name(int index);
std::string tetrio_key_name(int index);

} // namespace keyboard_inspector
