#include "file_list.h"
#include "datadir.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace filelist
{
    namespace
    {
        constexpr uint32_t K[64] = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138,
            0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70,
            0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa,
            0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

        uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        struct Sha256
        {
            uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                0x5be0cd19 };
            uint8_t block[64];
            size_t used = 0;
            uint64_t length = 0;

            void Compress(const uint8_t* data)
            {
                uint32_t w[64];
                for (int i = 0; i < 16; i++)
                    w[i] = uint32_t(data[i * 4]) << 24 | uint32_t(data[i * 4 + 1]) << 16 |
                           uint32_t(data[i * 4 + 2]) << 8 | data[i * 4 + 3];
                for (int i = 16; i < 64; i++)
                {
                    uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
                for (int i = 0; i < 64; i++)
                {
                    uint32_t t1 = k + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
                    uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                    k = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
                h[5] += f;
                h[6] += g;
                h[7] += k;
            }

            void Update(const uint8_t* data, size_t size)
            {
                length += size;
                if (used != 0)
                {
                    size_t take = std::min(size, sizeof(block) - used);
                    std::copy(data, data + take, block + used);
                    used += take;
                    data += take;
                    size -= take;
                    if (used < sizeof(block))
                        return;
                    Compress(block);
                    used = 0;
                }
                // Whole blocks straight from the caller's buffer: no copy for the bulk of a file.
                for (; size >= sizeof(block); data += sizeof(block), size -= sizeof(block))
                    Compress(data);
                std::copy(data, data + size, block);
                used = size;
            }

            std::string Finish()
            {
                uint64_t bits = length * 8;
                uint8_t tail[72] = { 0x80 };
                size_t pad = (used < 56 ? 56 : 120) - used;
                for (int i = 0; i < 8; i++)
                    tail[pad + size_t(i)] = uint8_t(bits >> (56 - 8 * i));
                Update(tail, pad + 8);
                char hex[65];
                for (int i = 0; i < 8; i++)
                    snprintf(hex + i * 8, 9, "%08x", h[i]);
                return std::string(hex, 64);
            }
        };

        bool IsHex64(std::string_view text)
        {
            return text.size() == 64 && std::all_of(text.begin(), text.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        // A listed path must stay inside the folder.
        bool Safe(std::string_view path)
        {
            if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
                path.find(':') != std::string_view::npos)
                return false;
            size_t start = 0;
            while (start <= path.size())
            {
                size_t end = std::min(path.find('/', start), path.size());
                std::string_view part = path.substr(start, end - start);
                if (part.empty() || part == "." || part == "..")
                    return false;
                start = end + 1;
            }
            return true;
        }

        std::string ReadAll(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        bool WriteAll(const fs::path& file, const std::string& text)
        {
            fs::path temporary = file;
            temporary += ".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out << text;
                if (!out.flush())
                    return false;
            }
            std::error_code error;
            fs::rename(temporary, file, error);
            if (error)
                fs::remove(temporary, error);
            return !error;
        }

        // What a file looks like without reading it: its size and modification time.
        struct Stamp
        {
            uint64_t size = 0;
            int64_t time = 0;
            bool operator==(const Stamp&) const = default;
        };

        bool StampOf(const fs::path& file, Stamp& stamp)
        {
            std::error_code error;
            if (!fs::is_regular_file(file, error))
                return false;
            stamp.size = fs::file_size(file, error);
            if (error)
                return false;
            stamp.time = int64_t(fs::last_write_time(file, error).time_since_epoch().count());
            return !error;
        }

        // files.sha256.verified: "<hash> <size> <time> <path>" per line.
        struct Verified
        {
            std::string sha256;
            Stamp stamp;
        };

        std::map<std::string, Verified> ReadVerified(const fs::path& folder)
        {
            std::map<std::string, Verified> verified;
            std::istringstream lines(ReadAll(folder / VerifiedName));
            for (std::string line; std::getline(lines, line);)
            {
                std::istringstream fields(line);
                Verified entry;
                std::string path;
                if (fields >> entry.sha256 >> entry.stamp.size >> entry.stamp.time && fields.get() == ' ' &&
                    std::getline(fields, path))
                    verified[path] = entry;
            }
            return verified;
        }

        void WriteVerified(const fs::path& folder, const std::map<std::string, Verified>& verified)
        {
            std::string text;
            for (const auto& [path, entry] : verified)
                text += entry.sha256 + " " + std::to_string(entry.stamp.size) + " " + std::to_string(entry.stamp.time) +
                        " " + path + "\n";
            WriteAll(folder / VerifiedName, text); // best effort: without it the next check reads everything
        }
    }

    std::string Sha256OfFile(const fs::path& file, const std::atomic<bool>* cancel)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return {};
        Sha256 sha;
        std::vector<char> buffer(1 << 20);
        while (in)
        {
            if (cancel != nullptr && *cancel)
                return {};
            in.read(buffer.data(), std::streamsize(buffer.size()));
            sha.Update(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(in.gcount()));
        }
        if (in.bad() || !in.eof())
            return {};
        return sha.Finish();
    }

    bool Parse(std::string_view text, std::vector<Entry>& entries)
    {
        entries.clear();
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = std::min(text.find('\n', start), text.size());
            std::string_view line = text.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty() || line.front() == '#')
                continue;
            // "<hash>  <path>"; shasum's binary mode writes "<hash> *<path>".
            if (line.size() < 67 || !IsHex64(line.substr(0, 64)) || line[64] != ' ' ||
                (line[65] != ' ' && line[65] != '*'))
                return false;
            std::string_view path = line.substr(66);
            if (!Safe(path))
                return false;
            entries.push_back({ std::string(line.substr(0, 64)), std::string(path) });
        }
        return true;
    }

    std::string Format(const std::vector<Entry>& entries)
    {
        std::string text;
        for (const Entry& entry : entries)
            text += entry.sha256 + "  " + entry.path + "\n";
        return text;
    }

    bool Write(const fs::path& folder, const std::vector<std::string>& paths, std::string& error)
    {
        std::vector<Entry> entries;
        std::map<std::string, Verified> verified;
        for (const std::string& path : paths)
        {
            if (!Safe(path))
            {
                error = "not a path inside the folder: " + path;
                return false;
            }
            const fs::path file = folder / datadir::FromText(path);
            Verified entry;
            if (!StampOf(file, entry.stamp) || (entry.sha256 = Sha256OfFile(file)).empty())
            {
                error = "can't read " + path;
                return false;
            }
            entries.push_back({ entry.sha256, path });
            verified[path] = entry;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
        std::error_code ignored;
        fs::remove(folder / VerifiedName, ignored); // never a record of an older list
        if (!WriteAll(folder / ListName, Format(entries)))
        {
            error = "can't write " + datadir::Text(folder / ListName);
            return false;
        }
        WriteVerified(folder, verified);
        return true;
    }

    Result Check(const fs::path& folder, const std::atomic<bool>* cancel)
    {
        Result result;
        std::error_code error;
        const fs::path listFile = folder / ListName;
        if (!fs::is_regular_file(listFile, error))
            return result;
        result.listed = true;
        std::vector<Entry> entries;
        if (!Parse(ReadAll(listFile), entries))
        {
            result.problems.push_back({ Problem::Kind::Changed, ListName });
            return result;
        }
        result.files = entries.size();
        const std::map<std::string, Verified> before = ReadVerified(folder);
        std::map<std::string, Verified> after;
        for (const Entry& entry : entries)
        {
            if (cancel != nullptr && *cancel)
            {
                result.cancelled = true;
                return result;
            }
            const fs::path file = folder / datadir::FromText(entry.path);
            Verified now;
            if (!StampOf(file, now.stamp))
            {
                result.problems.push_back({ Problem::Kind::Missing, entry.path });
                continue;
            }
            auto known = before.find(entry.path);
            if (known != before.end() && known->second.sha256 == entry.sha256 && known->second.stamp == now.stamp)
            {
                after[entry.path] = known->second;
                continue;
            }
            now.sha256 = Sha256OfFile(file, cancel);
            result.hashed++;
            result.hashedBytes += now.stamp.size;
            if (now.sha256.empty() && cancel != nullptr && *cancel)
            {
                result.cancelled = true;
                return result;
            }
            if (now.sha256 != entry.sha256)
            {
                result.problems.push_back(
                    { now.sha256.empty() ? Problem::Kind::Missing : Problem::Kind::Changed, entry.path });
                continue;
            }
            after[entry.path] = now;
        }
        if (after.size() != before.size() || result.hashed != 0)
            WriteVerified(folder, after);
        return result;
    }
}
