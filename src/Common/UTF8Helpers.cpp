#include <Common/UTF8Helpers.h>
#include <Common/StringUtils.h>
#include <Poco/UTF8Encoding.h>

#include <widechar_width.h>
#include <bit>

namespace DB
{
namespace UTF8
{

// based on https://bjoern.hoehrmann.de/utf-8/decoder/dfa/
// Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions: The above copyright
// notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

static const UInt8 TABLE[] =
{
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
    0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
    0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
    0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
    1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
    1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
    1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

struct UTF8Decoder
{
    enum
    {
        ACCEPT = 0,
        REJECT = 1
    };

    UInt32 decode(UInt8 byte)
    {
        UInt32 type = TABLE[byte];
        codepoint = (state != ACCEPT) ? (byte & 0x3fu) | (codepoint << 6) : (0xff >> type) & (byte);
        state = TABLE[256 + state * 16 + type];
        return state;
    }

    void reset()
    {
        state = ACCEPT;
        codepoint = 0xfffdU;
    }

    UInt8 state {ACCEPT};
    UInt32 codepoint {0};
};

static int wcwidth(wchar_t wc)
{
    int width = widechar_wcwidth(wc);
    switch (width)
    {
        case widechar_nonprint:
        case widechar_combining:
        case widechar_unassigned:
            return 0;
        case widechar_ambiguous:
        case widechar_private_use:
        case widechar_widened_in_9:
            return 1;
        default:
            return width;
    }
}


namespace
{

enum ComputeWidthMode
{
    Width,              /// Calculate and return visible width
    BytesBeforeLimit    /// Calculate and return the maximum number of bytes when substring fits in visible width.
};

template <ComputeWidthMode mode>
size_t computeWidthImpl(const UInt8 * data, size_t size, size_t prefix, size_t limit) noexcept
{
    UTF8Decoder decoder;
    bool is_escape_sequence = false;
    size_t width = 0;
    size_t rollback = 0;
    for (size_t i = 0; i < size; ++i)
    {
        /// Quickly skip regular ASCII

#if defined(__SSE2__)
        const auto lower_bound = _mm_set1_epi8(32);
        const auto upper_bound = _mm_set1_epi8(126);

        while (i + 15 < size)
        {
            if (is_escape_sequence)
                break;

            __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&data[i]));

            const uint16_t non_regular_width_mask = _mm_movemask_epi8(
                _mm_or_si128(
                    _mm_cmplt_epi8(bytes, lower_bound),
                    _mm_cmpgt_epi8(bytes, upper_bound)));

            if (non_regular_width_mask)
            {
                auto num_regular_chars = std::countr_zero(non_regular_width_mask);
                width += num_regular_chars;
                i += num_regular_chars;
                break;
            }

            i += 16;
            width += 16;
        }
#endif

        while (i < size && isPrintableASCII(data[i]))
        {
            bool ignore_width = is_escape_sequence && (isCSIParameterByte(data[i]) || isCSIIntermediateByte(data[i]));

            if (ignore_width || (data[i] == '[' && is_escape_sequence))
            {
                /// don't count the width
            }
            else if (is_escape_sequence && isCSIFinalByte(data[i]))
            {
                is_escape_sequence = false;
            }
            else
            {
                ++width;
            }
            ++i;
        }

        /// Now i points to position in bytes after regular ASCII sequence
        /// and if width > limit, then (width - limit) is the number of extra ASCII characters after width limit.

        if (mode == BytesBeforeLimit && width > limit)
            return i - (width - limit);

        if (i < size)
        {
            switch (decoder.decode(data[i]))
            {
                case UTF8Decoder::REJECT:
                {
                    decoder.reset();
                    // invalid sequences seem to have zero width in modern terminals
                    // tested in libvte-based, alacritty, urxvt and xterm
                    i -= rollback;
                    rollback = 0;
                    break;
                }
                case UTF8Decoder::ACCEPT:
                {
                    // TODO: multiline support for '\n'

                    // special treatment for '\t' and for ESC
                    size_t next_width = width;
                    if (decoder.codepoint == '\x1b')
                        is_escape_sequence = true;
                    else if (decoder.codepoint == '\t')
                        next_width += 8 - (prefix + width) % 8;
                    else
                        next_width += wcwidth(decoder.codepoint);

                    if (mode == BytesBeforeLimit && next_width > limit)
                        return i - rollback;
                    width = next_width;

                    rollback = 0;
                    break;
                }
                // continue if we meet other values here
                default:
                    ++rollback;
            }
        }
    }

    // no need to handle trailing sequence as they have zero width
    return (mode == BytesBeforeLimit) ? size : width;
}

}

size_t computeWidth(const UInt8 * data, size_t size, size_t prefix) noexcept
{
    return computeWidthImpl<Width>(data, size, prefix, 0);
}

size_t computeBytesBeforeWidth(const UInt8 * data, size_t size, size_t prefix, size_t limit) noexcept
{
    return computeWidthImpl<BytesBeforeLimit>(data, size, prefix, limit);
}


size_t convertCodePointToUTF8(int code_point, char * out_bytes, size_t out_length)
{
    static const Poco::UTF8Encoding utf8;
    int res = utf8.convert(
        code_point,
        reinterpret_cast<uint8_t *>(out_bytes),
        static_cast<int>(out_length));
    assert(res >= 0);
    return res;
}

std::optional<uint32_t> convertUTF8ToCodePoint(const char * in_bytes, size_t in_length)
{
    static const Poco::UTF8Encoding utf8;
    int res = utf8.queryConvert(
        reinterpret_cast<const uint8_t *>(in_bytes),
        static_cast<int>(in_length));

    if (res >= 0)
        return res;
    return {};
}

}
}
