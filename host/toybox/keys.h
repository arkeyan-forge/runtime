#pragma once

#include <SDL3/SDL.h>
#include <string>

namespace toybox
{
    // Shared interpretation of the saved Toy Box keys, including the legacy Tab alias.
    struct Keys
    {
        SDL_Scancode open = SDL_SCANCODE_F6, add = SDL_SCANCODE_F7, clear = SDL_SCANCODE_F8;
        bool tab = true;

        void Set(const std::string (&names)[3], bool tabAlias)
        {
            SDL_Scancode* codes[] = { &open, &add, &clear };
            const SDL_Scancode defaults[] = { SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8 };
            for (int i = 0; i < 3; i++)
            {
                SDL_Scancode code = SDL_GetScancodeFromName(names[i].c_str());
                *codes[i] = code == SDL_SCANCODE_UNKNOWN ? defaults[i] : code;
            }
            tab = tabAlias;
        }

        bool Hold(SDL_Scancode code) const { return code == (tab ? SDL_SCANCODE_TAB : open); }
        bool Open(SDL_Scancode code) const { return tab && code == open; }
    };
}
