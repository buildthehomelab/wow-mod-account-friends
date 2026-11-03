// c++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/text.cpp -o /tmp/text && /tmp/text
#include "AccountFriendsText.h"

#include <cstdio>
#include <string>

using namespace AccountFriends::Text;

static int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

int main()
{
    // Tag names: 2-12 letters and digits, starting with a letter.
    CHECK(ValidTagName("Pawster"));
    CHECK(ValidTagName("Ab"));
    CHECK(ValidTagName("Panda2"));
    CHECK(ValidTagName("Abcdefghijkl"));
    CHECK(!ValidTagName("A"));
    CHECK(!ValidTagName("Abcdefghijklm"));
    CHECK(!ValidTagName("2Panda"));
    CHECK(!ValidTagName("Red Panda"));
    CHECK(!ValidTagName("Pan#da"));
    CHECK(!ValidTagName("P\xC3\xA4nda"));

    // Tags.
    auto tag = ParseTag("  Pawster#1234 ");
    CHECK(tag && tag->name == "Pawster" && tag->code == 1234);
    CHECK(!ParseTag("Pawster"));
    CHECK(!ParseTag("Pawster#123"));
    CHECK(!ParseTag("Pawster#12345"));
    CHECK(!ParseTag("Pawster#0999"));
    CHECK(!ParseTag("Pawster#12a4"));
    CHECK(!ParseTag("#1234"));
    CHECK(!ParseTag("Paw ster#1234"));
    CHECK(FormatTag("Pawster", 1234) == "Pawster#1234");

    // UTF-8.
    CHECK(ValidUtf8("caf\xC3\xA9"));
    CHECK(!ValidUtf8("caf\xC3"));
    CHECK(!ValidUtf8("\xC0\xAF"));
    CHECK(!ValidUtf8("\xED\xA0\x80" "x\x80"));
    std::string const accents = "\xC3\xA9\xC3\xA9";  // two 2-byte characters
    CHECK(Utf8Cut(accents, 3) == 2);
    CHECK(Utf8Cut(accents, 4) == 4);
    CHECK(Utf8Cut("abc", 10) == 3);

    // Plain text: no control characters, tabs or pipes; trimmed; cut on a boundary.
    CHECK(CleanPlain("  hi\tthere|cffff0000red|r  ", 127) == std::string("hitherecffff0000redr"));
    CHECK(CleanPlain("   ", 127) == std::string());
    CHECK(!CleanPlain("bad\xC3", 127));
    CHECK(CleanPlain(std::string(10, 'a') + accents, 11) == std::string(10, 'a'));
    CHECK(CleanPlain(std::string(200, 'x'), 127)->size() == 127);

    // Whispers.
    CHECK(ValidWhisper("hello |cff9d9d9d|Hitem:1:0:0:0:0:0:0:0:1|h[x]|h|r"));
    CHECK(!ValidWhisper(""));
    CHECK(!ValidWhisper("   "));
    CHECK(!ValidWhisper("tab\there"));
    CHECK(!ValidWhisper("line\nbreak"));
    CHECK(!ValidWhisper(std::string(256, 'x')));
    CHECK(ValidWhisper(std::string(255, 'x')));

    // Fields: the last keeps the rest.
    auto fields = SplitFields("A\tPawster#1234\thello\tthere", '\t', 3);
    CHECK(fields.size() == 3 && fields[0] == "A" && fields[1] == "Pawster#1234" && fields[2] == "hello\tthere");
    fields = SplitFields("H", '\t', 3);
    CHECK(fields.size() == 1 && fields[0] == "H");
    fields = SplitFields("X\t", '\t', 3);
    CHECK(fields.size() == 2 && fields[1].empty());

    // Chunks never split a character and always cover the text.
    std::string long_text(219, 'a');
    long_text += accents;  // byte 220 is the second half of an é
    auto chunks = Chunks(long_text);
    CHECK(chunks.size() == 2 && chunks[0].size() == 219 && chunks[0] + chunks[1] == long_text);
    CHECK(Chunks("").size() == 1 && Chunks("")[0].empty());
    CHECK(Chunks(std::string(220, 'x')).size() == 1);
    CHECK(Chunks(std::string(221, 'x')).size() == 2);

    // Ids.
    CHECK(ParseId("42") == std::optional<uint32_t>(42));
    CHECK(!ParseId(""));
    CHECK(!ParseId("4x"));
    CHECK(!ParseId("-1"));
    CHECK(!ParseId("1234567890"));

    // Rate limit: capacity, then one per refill, across a getMSTime wrap.
    RateLimit limit(3, 1000);
    uint32_t t = 0xFFFFFF00u;
    CHECK(limit.Allow(t) && limit.Allow(t) && limit.Allow(t));
    CHECK(!limit.Allow(t + 10));
    CHECK(limit.Allow(t + 1000));  // wraps past zero
    CHECK(!limit.Allow(t + 1001));
    CHECK(limit.Allow(t + 10000) && limit.Allow(t + 10000) && limit.Allow(t + 10000) && !limit.Allow(t + 10000));

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("text tests passed");
    return 0;
}
