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

#include "base/files/file_util.h"

#include "build/build_config.h"

#include <fstream>

#if defined(OS_WIN)
#include "base/win/scoped_object.h"
#include <Windows.h>
#endif // defined(OS_WIN)

namespace base {

namespace {

const wchar_t kTempSuffix[] = L".tmp";

//--------------------------------------------------------------------------------------------------
// Asks the operating system to put the file on the disk. Closing the stream only hands the content
// to the system, which is enough to survive a crash of this process but not a loss of power: the
// rename could reach the disk before the content it is supposed to publish.
bool flushToDisk(const std::filesystem::path& filename)
{
#if defined(OS_WIN)
    win::ScopedHandle file(CreateFileW(filename.c_str(),
                                       GENERIC_WRITE,
                                       FILE_SHARE_READ,
                                       nullptr,
                                       OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL,
                                       nullptr));
    if (!file.isValid())
        return false;

    return !!FlushFileBuffers(file.get());
#else // defined(OS_WIN)
    return true;
#endif // !defined(OS_WIN)
}

//--------------------------------------------------------------------------------------------------
template <class Container>
bool readFileT(const std::filesystem::path& filename, Container* buffer)
{
    if (!buffer)
        return false;

    std::ifstream stream;
    stream.open(filename, std::ifstream::binary | std::ifstream::in);
    if (!stream.is_open())
        return false;

    stream.seekg(0, stream.end);
    size_t size = static_cast<size_t>(stream.tellg());
    stream.seekg(0);

    buffer->clear();

    if (!size)
        return true;

    if (size >= buffer->max_size())
        return false;

    buffer->resize(size);

    stream.read(reinterpret_cast<char*>(buffer->data()), buffer->size());
    return !stream.fail();
}

} // namespace

//--------------------------------------------------------------------------------------------------
bool writeFile(const std::filesystem::path& filename, const void* data, size_t size)
{
    if (!data)
        return false;

    std::ofstream stream;
    stream.open(filename, std::ofstream::binary | std::ofstream::out | std::ofstream::trunc);
    if (!stream.is_open())
        return false;

    stream.seekp(0);
    stream.write(reinterpret_cast<const char*>(data), size);

    return !stream.fail();
}

//--------------------------------------------------------------------------------------------------
bool writeFile(const std::filesystem::path& filename, const ByteArray& buffer)
{
    return writeFile(filename, buffer.data(), buffer.size());
}

//--------------------------------------------------------------------------------------------------
bool writeFile(const std::filesystem::path& filename, std::string_view buffer)
{
    return writeFile(filename, buffer.data(), buffer.size());
}

//--------------------------------------------------------------------------------------------------
bool writeFileAtomically(const std::filesystem::path& filename, const void* data, size_t size)
{
    if (!data)
        return false;

    // The temporary is put next to the target. A rename is only atomic within one file system, so
    // the temporary directory of the system would not do: between two of them the rename turns
    // into a copy, and a copy is exactly what must not happen here.
    std::filesystem::path temp_filename = filename;
    temp_filename += kTempSuffix;

    std::error_code ignored_error_code;

    if (!writeFile(temp_filename, data, size) || !flushToDisk(temp_filename))
    {
        std::filesystem::remove(temp_filename, ignored_error_code);
        return false;
    }

    std::error_code error_code;
    std::filesystem::rename(temp_filename, filename, error_code);
    if (error_code)
    {
        // The target still holds what it held before. Leaving the temporary next to it would only
        // add a second file that looks like the first one.
        std::filesystem::remove(temp_filename, ignored_error_code);
        return false;
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
bool writeFileAtomically(const std::filesystem::path& filename, const ByteArray& buffer)
{
    return writeFileAtomically(filename, buffer.data(), buffer.size());
}

//--------------------------------------------------------------------------------------------------
bool writeFileAtomically(const std::filesystem::path& filename, std::string_view buffer)
{
    return writeFileAtomically(filename, buffer.data(), buffer.size());
}

//--------------------------------------------------------------------------------------------------
bool readFile(const std::filesystem::path& filename, ByteArray* buffer)
{
    return readFileT<ByteArray>(filename, buffer);
}

//--------------------------------------------------------------------------------------------------
bool readFile(const std::filesystem::path& filename, std::string* buffer)
{
    return readFileT<std::string>(filename, buffer);
}

} // namespace base
