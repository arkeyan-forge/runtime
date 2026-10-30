#pragma once

#include <cstdint>
#include <functional>
#include <string>

// printf for the guest, with the Microsoft CRT's conversions (%S is a wide string, I64 a 64-bit length). `next` yields
// the 8-byte argument slots in order: integers and pointers in the low word, doubles in the whole slot. Strings are
// read from guest memory.
std::string FormatGuest(const char* format, const std::function<uint64_t()>& next);
