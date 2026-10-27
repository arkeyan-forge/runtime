#include "game.h"
#include <algorithm>
#include <cctype>

std::string game::MovieName(std::string_view path)
{
    size_t start = path.find_last_of("\\/");
    std::string name(start == std::string_view::npos ? path : path.substr(start + 1));
    size_t dot = name.rfind('.');
    if (dot != std::string::npos)
        name.resize(dot);
    for (char& c : name)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

bool game::IsIntroMovie(std::string_view path)
{
    const auto& intro = Current().introMovies;
    return std::find(intro.begin(), intro.end(), MovieName(path)) != intro.end();
}
