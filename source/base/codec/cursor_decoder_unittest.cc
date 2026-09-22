//
// Aspia Project
// Copyright (C) 2016-2024 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "base/codec/cursor_decoder.h"

#include "base/codec/cursor_encoder.h"
#include "base/desktop/mouse_cursor.h"
#include "proto/desktop.pb.h"

#include <random>

#include <gtest/gtest.h>

// For ZSTD_getFrameHeader, to see the window a frame declares.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace base {

namespace {

MouseCursor makeCursor(int width, int height, uint32_t seed)
{
    // Noise, so that zstd cannot shrink the image to nothing and has to use its window.
    std::mt19937 generator(seed);
    ByteArray image(static_cast<size_t>(width) * static_cast<size_t>(height) *
                    MouseCursor::kBytesPerPixel);
    for (auto& byte : image)
        byte = static_cast<uint8_t>(generator());

    return MouseCursor(std::move(image), Size(width, height), Point(0, 0));
}

// Compresses the way a host is free to, with any window it likes, and leaves the content size out
// of the frame so that the window is what the frame declares.
std::string compressWithWindow(const ByteArray& image, int window_log)
{
    std::unique_ptr<ZSTD_CCtx, size_t(*)(ZSTD_CCtx*)> ctx(ZSTD_createCCtx(), ZSTD_freeCCtx);
    ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_windowLog, window_log);
    ZSTD_CCtx_setParameter(ctx.get(), ZSTD_c_contentSizeFlag, 0);

    std::string output(ZSTD_compressBound(image.size()), '\0');
    ZSTD_inBuffer input = { image.data(), image.size(), 0 };
    ZSTD_outBuffer out = { output.data(), output.size(), 0 };

    // Fed as a stream: given everything in one call, zstd would learn the size and shrink the
    // window to fit it.
    size_t ret = ZSTD_compressStream2(ctx.get(), &out, &input, ZSTD_e_continue);
    EXPECT_FALSE(ZSTD_isError(ret));

    ZSTD_inBuffer no_more_input = { nullptr, 0, 0 };
    ret = ZSTD_compressStream2(ctx.get(), &out, &no_more_input, ZSTD_e_end);
    EXPECT_FALSE(ZSTD_isError(ret));
    EXPECT_EQ(ret, 0u);

    ZSTD_frameHeader header;
    EXPECT_EQ(ZSTD_getFrameHeader(&header, output.data(), out.pos), 0u);
    EXPECT_EQ(header.windowSize, 1ull << window_log);

    output.resize(out.pos);
    return output;
}

proto::CursorShape shapeWithData(int width, int height, std::string data)
{
    proto::CursorShape shape;
    shape.set_width(width);
    shape.set_height(height);
    shape.set_flags(proto::CursorShape::RESET_CACHE | 30);
    shape.set_data(std::move(data));
    return shape;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// What the encoder of 2.7 - the one in every host - produces must keep decoding, up to the largest
// cursor Windows makes.
TEST(cursor_decoder_test, decodes_what_the_encoder_produces)
{
    const int kSizes[] = { 32, 64, 128, 256 };

    for (int size : kSizes)
    {
        MouseCursor cursor = makeCursor(size, size, static_cast<uint32_t>(size));

        CursorEncoder encoder;
        proto::CursorShape shape;
        ASSERT_TRUE(encoder.encode(cursor, &shape)) << size;

        ZSTD_frameHeader header;
        ASSERT_EQ(ZSTD_getFrameHeader(&header, shape.data().data(), shape.data().size()), 0u);
        EXPECT_LE(header.windowSize, 1ull << 22) << size;

        CursorDecoder decoder;
        std::shared_ptr<MouseCursor> decoded = decoder.decode(shape);
        ASSERT_TRUE(decoded) << size;
        EXPECT_EQ(decoded->constImage(), cursor.constImage()) << size;
    }
}

//--------------------------------------------------------------------------------------------------
TEST(cursor_decoder_test, decodes_a_frame_within_the_window_limit)
{
    MouseCursor cursor = makeCursor(16, 16, 1);

    CursorDecoder decoder;
    std::shared_ptr<MouseCursor> decoded =
        decoder.decode(shapeWithData(16, 16, compressWithWindow(cursor.constImage(), 22)));

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->constImage(), cursor.constImage());
}

//--------------------------------------------------------------------------------------------------
// A tiny frame that declares a 128 MB window would make the decoder set that much aside.
TEST(cursor_decoder_test, refuses_a_frame_that_asks_for_a_large_window)
{
    MouseCursor cursor = makeCursor(16, 16, 1);

    CursorDecoder decoder;
    EXPECT_FALSE(decoder.decode(shapeWithData(16, 16, compressWithWindow(cursor.constImage(), 27))));
}

//--------------------------------------------------------------------------------------------------
// A host that announces a small cursor and sends the data of a large one. The decode loop runs
// while input remains, and would spin with the console frozen if zstd kept taking calls that
// cannot move; zstd 1.5 reports that as an error instead, and this holds it to it.
TEST(cursor_decoder_test, refuses_data_larger_than_the_announced_cursor)
{
    MouseCursor cursor = makeCursor(64, 64, 2);

    CursorDecoder decoder;
    EXPECT_FALSE(decoder.decode(shapeWithData(4, 4, compressWithWindow(cursor.constImage(), 22))));
}

} // namespace base
