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

#ifndef BASE_FILES_FILE_UTIL_H
#define BASE_FILES_FILE_UTIL_H

#include "base/memory/byte_array.h"

#include <filesystem>

namespace base {

bool writeFile(const std::filesystem::path& filename, const void* data, size_t size);
bool writeFile(const std::filesystem::path& filename, const ByteArray& buffer);
bool writeFile(const std::filesystem::path& filename, std::string_view buffer);

// Writes the file the way writeFile does, but never leaves it half-written. The content goes to a
// temporary file next to the target and that file is renamed over it, so at every moment the name
// points either at the whole previous content or at the whole new one. A failure anywhere leaves
// the previous content untouched and takes the temporary with it.
//
// This is what the callers that keep the only copy of something must use. writeFile truncates the
// target before it has anything to put in its place, so an interrupted write destroys the file.
bool writeFileAtomically(const std::filesystem::path& filename, const void* data, size_t size);
bool writeFileAtomically(const std::filesystem::path& filename, const ByteArray& buffer);
bool writeFileAtomically(const std::filesystem::path& filename, std::string_view buffer);

bool readFile(const std::filesystem::path& filename, ByteArray* buffer);
bool readFile(const std::filesystem::path& filename, std::string* buffer);

} // namespace base

#endif // BASE_FILES_FILE_UTIL_H
