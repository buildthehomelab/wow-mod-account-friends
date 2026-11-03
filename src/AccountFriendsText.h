/*
 * mod-account-friends: text helpers shared by the module and its tests. Nothing here needs the
 * core, so tests/text.cpp builds it on its own.
 *
 * Released under the MIT License, see LICENSE.
 */

#ifndef MOD_ACCOUNT_FRIENDS_TEXT_H
#define MOD_ACCOUNT_FRIENDS_TEXT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AccountFriends::Text
{
    constexpr std::size_t TagNameMin = 2;
    constexpr std::size_t TagNameMax = 12;
    constexpr uint16_t TagCodeMin = 1000;
    constexpr uint16_t TagCodeMax = 9999;
    constexpr std::size_t NoteMax = 127;       // the stock note dialog allows 127 letters
    constexpr std::size_t BroadcastMax = 127;
    constexpr std::size_t RequestMessageMax = 127;
    constexpr std::size_t WhisperMax = 255;    // one chat line

    // Payload room in one addon message: 255 bytes minus "ACFR\t" and a command like "W\t<id>\t".
    constexpr std::size_t ChunkMax = 220;

    struct Tag
    {
        std::string name;
        uint16_t code = 0;
    };

    inline bool IsAsciiLetter(char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    inline bool IsAsciiDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    // 2-12 letters and digits, starting with a letter.
    inline bool ValidTagName(std::string_view name)
    {
        if (name.size() < TagNameMin || name.size() > TagNameMax || !IsAsciiLetter(name.front()))
            return false;
        for (char c : name)
            if (!IsAsciiLetter(c) && !IsAsciiDigit(c))
                return false;
        return true;
    }

    // "Name#1234" (case kept as typed; the database compares without case).
    inline std::optional<Tag> ParseTag(std::string_view text)
    {
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);
        while (!text.empty() && text.back() == ' ')
            text.remove_suffix(1);

        std::size_t const hash = text.find('#');
        if (hash == std::string_view::npos)
            return std::nullopt;
        std::string_view const name = text.substr(0, hash);
        std::string_view const digits = text.substr(hash + 1);
        if (!ValidTagName(name) || digits.size() != 4)
            return std::nullopt;

        uint16_t code = 0;
        for (char c : digits)
        {
            if (!IsAsciiDigit(c))
                return std::nullopt;
            code = uint16_t(code * 10 + (c - '0'));
        }
        if (code < TagCodeMin)
            return std::nullopt;
        return Tag{ std::string(name), code };
    }

    inline std::string FormatTag(std::string_view name, uint16_t code)
    {
        return std::string(name) + "#" + std::to_string(code);
    }

    // Length of the UTF-8 sequence starting with `lead`, or 0 when `lead` can't start one.
    inline std::size_t Utf8Length(unsigned char lead)
    {
        if (lead < 0x80)
            return 1;
        if (lead >= 0xC2 && lead <= 0xDF)
            return 2;
        if (lead >= 0xE0 && lead <= 0xEF)
            return 3;
        if (lead >= 0xF0 && lead <= 0xF4)
            return 4;
        return 0;
    }

    inline bool ValidUtf8(std::string_view text)
    {
        for (std::size_t i = 0; i < text.size();)
        {
            std::size_t const length = Utf8Length(static_cast<unsigned char>(text[i]));
            if (!length || i + length > text.size())
                return false;
            for (std::size_t j = 1; j < length; ++j)
                if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80)
                    return false;
            i += length;
        }
        return true;
    }

    // The longest prefix of `text` of at most `maxBytes` that doesn't cut a UTF-8 sequence.
    inline std::size_t Utf8Cut(std::string_view text, std::size_t maxBytes)
    {
        if (text.size() <= maxBytes)
            return text.size();
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        return cut;
    }

    // Notes, broadcasts and request messages are plain text: no control characters, no tabs
    // (the protocol's separator) and no '|' (escape sequences: colours, links, textures).
    // Spaces are trimmed and the result is cut to `maxBytes` on a character boundary. Returns
    // nothing for broken UTF-8.
    inline std::optional<std::string> CleanPlain(std::string_view text, std::size_t maxBytes)
    {
        if (!ValidUtf8(text))
            return std::nullopt;
        std::string out;
        out.reserve(text.size());
        for (char c : text)
        {
            unsigned char const u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || c == '|')
                continue;
            out += c;
        }
        std::size_t const first = out.find_first_not_of(' ');
        if (first == std::string::npos)
            return std::string();
        out = out.substr(first, out.find_last_not_of(' ') - first + 1);
        out.resize(Utf8Cut(out, maxBytes));
        return out;
    }

    // Whispers may carry links (checked by the core's link validator in the module), but no
    // control characters or tabs.
    inline bool ValidWhisper(std::string_view text)
    {
        if (text.empty() || text.size() > WhisperMax || !ValidUtf8(text))
            return false;
        for (char c : text)
        {
            unsigned char const u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F)
                return false;
        }
        return text.find_first_not_of(' ') != std::string_view::npos;
    }

    // Splits at `sep` into at most `maxFields` fields; the last one keeps the rest (separators
    // included), so free text goes last.
    inline std::vector<std::string_view> SplitFields(std::string_view text, char sep, std::size_t maxFields)
    {
        std::vector<std::string_view> fields;
        while (fields.size() + 1 < maxFields)
        {
            std::size_t const pos = text.find(sep);
            if (pos == std::string_view::npos)
                break;
            fields.push_back(text.substr(0, pos));
            text.remove_prefix(pos + 1);
        }
        fields.push_back(text);
        return fields;
    }

    // Pieces of at most `maxBytes` each, cut on character boundaries, for text that doesn't fit
    // one addon message. Never empty: an empty text is one empty piece.
    inline std::vector<std::string> Chunks(std::string_view text, std::size_t maxBytes = ChunkMax)
    {
        std::vector<std::string> pieces;
        do
        {
            std::size_t const cut = Utf8Cut(text, maxBytes);
            pieces.emplace_back(text.substr(0, cut));
            text.remove_prefix(cut);
        } while (!text.empty());
        return pieces;
    }

    inline std::optional<uint32_t> ParseId(std::string_view text)
    {
        if (text.empty() || text.size() > 9)
            return std::nullopt;
        uint32_t value = 0;
        for (char c : text)
        {
            if (!IsAsciiDigit(c))
                return std::nullopt;
            value = value * 10 + uint32_t(c - '0');
        }
        return value;
    }

    // At most `capacity` actions, refilled at one per `refillMs`.
    class RateLimit
    {
    public:
        RateLimit(uint32_t capacity, uint32_t refillMs) : _capacity(capacity), _refillMs(refillMs), _tokens(capacity) { }

        bool Allow(uint32_t nowMs)
        {
            if (!_started)
            {
                _started = true;
                _lastMs = nowMs;
            }
            uint32_t const elapsed = nowMs - _lastMs;  // wraps like getMSTime
            if (elapsed >= _refillMs)
            {
                uint32_t const add = elapsed / _refillMs;
                _tokens = add >= _capacity - _tokens ? _capacity : _tokens + add;
                _lastMs += add * _refillMs;
            }
            if (!_tokens)
                return false;
            --_tokens;
            return true;
        }

    private:
        uint32_t _capacity;
        uint32_t _refillMs;
        uint32_t _tokens;
        uint32_t _lastMs = 0;
        bool _started = false;
    };
}

#endif
