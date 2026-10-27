#pragma once

#include <string_view>

// The game's languages, by code. The disc has ten: en, fr, de, es, it, nl, sv, no, da, fi.
// Set by the host before the title starts; false for a code it doesn't know. (The voices' language is the game's own:
// game.h, Adapter::SetVoiceLanguage.)

// Text (menus, subtitles, localized movies): the console language the title reads (kernel/xconfig.cpp). English unless
// the host picks another. The other console languages (ja, ko, zh-tw, pt, zh-cn, pl, ru) are accepted too: the title
// has no text for them and shows English.
bool SetTextLanguage(std::string_view code);
// The text language's code ("fr" by default).
std::string_view TextLanguage();
