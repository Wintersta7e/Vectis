#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "code/properties_reader.h"

namespace {

using vectis::code::properties::parse_properties;
using vectis::code::properties::PropertiesEntry;

[[nodiscard, maybe_unused]] const PropertiesEntry* find(const std::vector<PropertiesEntry>& entries,
                                                        std::string_view key)
{
    for (const auto& e : entries) {
        if (e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

TEST(PropertiesReaderTest, KeyEqualsValue)
{
    const auto entries = parse_properties("foo=bar\nbaz = qux\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "foo");
    EXPECT_EQ(entries[0].value, "bar");
    EXPECT_EQ(entries[0].line_start, 1);
    EXPECT_EQ(entries[1].key, "baz");
    EXPECT_EQ(entries[1].value, "qux");
    EXPECT_EQ(entries[1].line_start, 2);
}

TEST(PropertiesReaderTest, KeyColonValue)
{
    const auto entries = parse_properties("foo:bar\nbaz : qux\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "foo");
    EXPECT_EQ(entries[0].value, "bar");
    EXPECT_EQ(entries[1].key, "baz");
    EXPECT_EQ(entries[1].value, "qux");
}

TEST(PropertiesReaderTest, HandlesBackslashLineContinuation)
{
    const auto entries = parse_properties("multi=line1\\\nline2\\\nline3\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "multi");
    EXPECT_EQ(entries[0].value, "line1line2line3")
        << "trailing backslash + newline must concatenate without preserving the newline";
    EXPECT_EQ(entries[0].line_start, 1)
        << "logical line keeps the line number of its first physical line";
}

TEST(PropertiesReaderTest, ContinuationStripsLeadingWhitespaceOnNextLine)
{
    // Java spec: leading whitespace on a continuation line is dropped.
    // `key=line1\\\n  line2` => value `line1line2`, NOT `line1  line2`.
    const auto entries = parse_properties("multi=line1\\\n  line2\\\n\t\tline3\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "multi");
    EXPECT_EQ(entries[0].value, "line1line2line3")
        << "leading horizontal whitespace on continuation lines must be stripped";
}

TEST(PropertiesReaderTest, EvenBackslashCountDoesNotContinue)
{
    // Two trailing backslashes = an escaped backslash, NOT a continuation.
    const auto entries = parse_properties("a=x\\\\\nb=y\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "a");
    EXPECT_EQ(entries[0].value, "x\\\\")
        << "even-count trailing backslashes stay literal; no concatenation";
    EXPECT_EQ(entries[1].key, "b");
    EXPECT_EQ(entries[1].value, "y");
}

TEST(PropertiesReaderTest, IgnoresHashAndBangComments)
{
    const auto entries =
        parse_properties("# comment line\n! also a comment\nreal=value\n   # indented comment\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "real");
    EXPECT_EQ(entries[0].value, "value");
    EXPECT_EQ(entries[0].line_start, 3) << "comment lines do not consume line numbers";
}

TEST(PropertiesReaderTest, HandlesEscapedSeparatorInKey)
{
    const auto entries = parse_properties(R"(weird\=key=value
other\:key:foo
spaced\ key = bar
)");
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[0].key, "weird=key");
    EXPECT_EQ(entries[0].value, "value");
    EXPECT_EQ(entries[1].key, "other:key");
    EXPECT_EQ(entries[1].value, "foo");
    EXPECT_EQ(entries[2].key, "spaced key");
    EXPECT_EQ(entries[2].value, "bar");
}

TEST(PropertiesReaderTest, HandlesWhitespaceSeparator)
{
    const auto entries = parse_properties("key value\nanother\tx\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "key");
    EXPECT_EQ(entries[0].value, "value");
    EXPECT_EQ(entries[1].key, "another");
    EXPECT_EQ(entries[1].value, "x");
}

TEST(PropertiesReaderTest, EmptyValue)
{
    // `key=` and bare `keyonly` are both Java-legal: empty value.
    const auto entries = parse_properties("empty=\nbare\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "empty");
    EXPECT_EQ(entries[0].value, "");
    EXPECT_EQ(entries[1].key, "bare");
    EXPECT_EQ(entries[1].value, "");
}

TEST(PropertiesReaderTest, MultipleSeparatorsFirstWins)
{
    const auto entries = parse_properties("k=v=more=stuff\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "k");
    EXPECT_EQ(entries[0].value, "v=more=stuff");
}

TEST(PropertiesReaderTest, BlankLinesIgnored)
{
    const auto entries = parse_properties("\n\nfoo=bar\n\n\nbaz=qux\n\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].line_start, 3);
    EXPECT_EQ(entries[1].line_start, 6);
}

TEST(PropertiesReaderTest, TolerantOfTrailingBackslashAtEOF)
{
    // A physical line ending with a single `\` at EOF (no next line to
    // continue into) must not crash. OpenJDK's `Properties.load`
    // consumes the trailing `\` as a continuation marker even when EOF
    // follows; we match that behavior, so the value is "v" with the
    // trailing backslash stripped.
    const auto entries = parse_properties("k=v\\");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "k");
    EXPECT_EQ(entries[0].value, "v");
}

TEST(PropertiesReaderTest, HandlesUtf8BomAtStart)
{
    // A UTF-8 BOM must not become part of the first key.
    const auto entries = parse_properties("\xEF\xBB\xBFspring.config.import=foo.properties\n");
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries[0].key, "spring.config.import")
        << "leading UTF-8 BOM must be stripped before tokenizing";
    EXPECT_EQ(entries[0].value, "foo.properties");
}

TEST(PropertiesReaderTest, HandlesCrlfLineEndings)
{
    const auto entries = parse_properties("a=x\r\nb=y\r\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "a");
    EXPECT_EQ(entries[0].value, "x") << "trailing \\r must be stripped from physical line";
    EXPECT_EQ(entries[1].key, "b");
    EXPECT_EQ(entries[1].value, "y");
}

TEST(PropertiesReaderTest, TolerantOfMalformedUtf8Byte)
{
    // The parser is byte-oriented; an invalid UTF-8 byte mid-line must
    // not abort parsing — it just appears verbatim in the value, and
    // the following lines parse normally. Spec: "best-effort,
    // replacement-byte posture."
    std::string body;
    body += "good=value\n";
    body += "bad=";
    body.push_back(static_cast<char>(0xC3)); // leading byte without continuation
    body += "\nafter=ok\n";
    const auto entries = parse_properties(body);
    ASSERT_EQ(entries.size(), 3U) << "one bad byte must not abort the file";
    EXPECT_EQ(entries[0].key, "good");
    EXPECT_EQ(entries[1].key, "bad");
    EXPECT_EQ(entries[2].key, "after");
    EXPECT_EQ(entries[2].value, "ok");
}

TEST(PropertiesReaderTest, TrailingWhitespaceInValueIsPreserved)
{
    // Java's Properties.load keeps trailing whitespace in values, and
    // Phase 4 follows that — values are taken literally so a path like
    // `secrets.properties  ` does not silently match `secrets.properties`.
    // Pin the contract so a future refactor cannot silently flip it.
    const auto entries = parse_properties("k=v   \nx=  spaced  \n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "k");
    EXPECT_EQ(entries[0].value, "v   ") << "trailing spaces preserved verbatim";
    EXPECT_EQ(entries[1].key, "x");
    EXPECT_EQ(entries[1].value, "spaced  ")
        << "leading WS after the separator is consumed; trailing WS is not";
}

TEST(PropertiesReaderTest, MixedSeparatorsInOneFile)
{
    const auto entries = parse_properties("a=1\nb:2\nc 3\nd\t4\n");
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[0].key, "a");
    EXPECT_EQ(entries[0].value, "1");
    EXPECT_EQ(entries[1].key, "b");
    EXPECT_EQ(entries[1].value, "2");
    EXPECT_EQ(entries[2].key, "c");
    EXPECT_EQ(entries[2].value, "3");
    EXPECT_EQ(entries[3].key, "d");
    EXPECT_EQ(entries[3].value, "4");
}

TEST(PropertiesReaderTest, EmptyContinuationLineTerminatesLogicalLine)
{
    // `k=v\\\n\nrest=x\n` — line 1 continues, line 2 is empty (so
    // after WS-strip the logical line equals "k=v"), line 3 is its
    // own logical line. Pins the boundary where a continuation
    // followed by an empty physical line ends the first logical line.
    const auto entries = parse_properties("k=v\\\n\nrest=x\n");
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].key, "k");
    EXPECT_EQ(entries[0].value, "v");
    EXPECT_EQ(entries[1].key, "rest");
    EXPECT_EQ(entries[1].value, "x");
}

} // namespace
