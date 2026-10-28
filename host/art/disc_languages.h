#pragma once

// Which text and voice languages a game's disc holds, decided from the disc's file list (names only, no file is read).
// Skylauncher shows only these in a game's Language settings. Where each game marks a language: docs/engines/lux/languages.md
// ("What the disc holds").
#include <string>
#include <string_view>
#include <vector>

namespace art
{
    struct DiscLanguages
    {
        std::vector<std::string> text;  // settings codes (`en fr de es es-419 it nl pt sv no da fi`), in that order; empty: unknown
        std::vector<std::string> voice; // same, for the spoken lines
    };

    // `files`: the paths of the disc's files (any case, '/' or '\', a leading separator or none).
    //  - "ssa": text is a folder text/<eng|fre|ger|spa|ita|dut|swe|nor|dan|fin>/ with files, voices are audio/<folder>/voice.bnk.
    //  - "swapforce": archives/<name>_<code>.pak (en, fr on the USA disc; es, it on the European one), for text and voices alike.
    //  - "giants": the languages are packs inside each .bld file, not in file names, and its only known release holds all twelve.
    // A game this does not know, or a disc without any marker (a partial copy): both lists empty (unknown: show every language).
    DiscLanguages LanguagesOnDisc(std::string_view game, const std::vector<std::string>& files);

    // The lists as stored in a settings file: codes joined by commas ("en,fr"), and back. Splitting skips empty parts.
    std::string JoinLanguages(const std::vector<std::string>& codes);
    std::vector<std::string> SplitLanguages(std::string_view text);
}
