/* Junocash - String wrapper for DMI code
 * Adapted from XMRig's String class to use std::string
 * Copyright (c) 2025 Junocash developers
 * Copyright (c) 2018-2021 XMRig <https://github.com/xmrig>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 */

#ifndef JUNOCASH_DMI_STRING_H
#define JUNOCASH_DMI_STRING_H

#include <string>
#include <cstring>

namespace xmrig {

// Simple string wrapper that uses std::string internally
// Provides API compatibility with xmrig's String class
class String
{
public:
    inline String() = default;
    inline String(const char *str) : m_data(str ? str : "") {}
    inline String(const std::string &str) : m_data(str) {}
    inline String(const String &other) : m_data(other.m_data) {}
    inline String(String &&other) noexcept : m_data(std::move(other.m_data)) {}

    inline ~String() = default;

    inline bool isEmpty() const          { return m_data.empty(); }
    inline bool isNull() const           { return m_data.empty(); }
    inline bool isValid() const          { return !m_data.empty(); }
    inline const char *data() const      { return m_data.c_str(); }
    inline size_t size() const           { return m_data.size(); }

    inline bool operator!=(const char *str) const      { return m_data != (str ? str : ""); }
    inline bool operator!=(const String &other) const  { return m_data != other.m_data; }
    inline bool operator==(const char *str) const      { return m_data == (str ? str : ""); }
    inline bool operator==(const String &other) const  { return m_data == other.m_data; }
    inline operator const char*() const                { return m_data.c_str(); }
    inline String &operator=(const char *str)          { m_data = str ? str : ""; return *this; }
    inline String &operator=(const String &str)        { m_data = str.m_data; return *this; }
    inline String &operator=(String &&other) noexcept  { m_data = std::move(other.m_data); return *this; }

private:
    std::string m_data;
};

} /* namespace xmrig */

#endif /* JUNOCASH_DMI_STRING_H */
