#pragma once

// Which text and voice languages a game's disc holds, decided from the disc's file list and, for the game that needs it, the
// entry names of two or three of its archives (names only, no file content is read). Skylauncher shows only these in a game's
// Language settings. Where each game marks a language: docs/engines/lux/languages.md ("What the disc holds").
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace art
{
    struct DiscLanguages
    {
        // settings codes (`en fr de es es-419 it nl pt sv no da fi pl ru cs hu tr el ja ko zh`), in that order; empty: unknown
        std::vector<std::string> text;
        std::vector<std::string> voice; // same, for the spoken lines
    };

    // The names of the entries of the IGA archive (.arc or .bld) at `path` on the disc, as the archive stores them; empty when the
    // file is not there or is not an archive. The disc's reader gives it (alchemy_archive.h).
    using ArchiveNames = std::function<std::vector<std::string>(const std::string& path)>;

    // `files`: the paths of the disc's files (any case, '/' or '\', a leading separator or none).
    //  - "ssa": text is a folder text/<eng|fre|ger|spa|ita|dut|swe|nor|dan|fin>/ with files, voices are audio/<folder>/voice.bnk.
    //  - "swapforce": archives/<name>_<code>.pak (en, fr on the USA disc; es, it on the European one), for text and voices alike.
    //  - "giants": the languages are packs inside each .bld file, not in file names, and its only known release holds all twelve.
    //  - "trapteam": every file name is the same on every release; the text is the "<LANGUAGE>.pak" entries of character/Init_Setup.bld
    //    (permanent/global.bld when that gives none), the voices are the language folders ("<character>/german/vo_x.wav") of the
    //    entries of the first character archives, and English, which has no folder (its lines are the unmarked ones), when the text
    //    has it and the archives hold voice lines. Needs `archive`; without it both lists are empty.
    // A game this does not know, or a disc without any marker (a partial copy): both lists empty (unknown: show every language).
    DiscLanguages LanguagesOnDisc(
        std::string_view game, const std::vector<std::string>& files, const ArchiveNames& archive = {});

    // The lists as stored in a settings file: codes joined by commas ("en,fr"), and back. Splitting skips empty parts.
    std::string JoinLanguages(const std::vector<std::string>& codes);
    std::vector<std::string> SplitLanguages(std::string_view text);
}
