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

#ifndef CONSOLE_BOOK_SYNC_KEY_H
#define CONSOLE_BOOK_SYNC_KEY_H

#include <string>

namespace console {

// The key the shared address book is encrypted with, and the only thing standing between the
// router and the passwords of every client machine.
//
// The router stores records it cannot read: what it holds is the shape of the tree, the guids and
// the times, and a sealed payload for each record. That is deliberate. The server is reachable
// from the internet, and a copy of its database - taken from the machine or from a backup - must
// not hand over the passwords with it.
//
// The key is derived from a passphrase the department agrees on and a salt the router keeps, one
// per book. The same passphrase alone would not do: every address book file carries a random salt
// of its own, so seven people typing the same word would end up with seven different keys and
// could not read each other's records.
//
// What this does not protect against is spelled out in the design: the router can withhold records
// or serve stale ones, and somebody who has already read the book keeps what they read. Both are
// accepted.

// Length of the salt stored with the book on the router.
constexpr size_t kSyncSaltSize = 32;

// Makes a salt for a new book.
std::string createSyncSalt();

// Derives the key from the passphrase and the salt. Returns an empty string when the salt is not
// of the expected size - a short salt would weaken the result silently, and silence is the one
// thing that must not happen here.
std::string deriveSyncKey(std::string_view passphrase, std::string_view salt);

// A value stored with the book that tells the right key from a wrong one.
//
// Without it, somebody who mistyped the passphrase would derive a different key, seal records with
// it and send them: readable to nobody, and it would not be apparent whose fault it was or when it
// happened. With it, the console can tell before it does anything at all.
std::string createKeyVerifier(std::string_view key);
bool checkKeyVerifier(std::string_view key, std::string_view verifier);

// Seals a record for the router, and opens what came back. The router never sees either the key or
// what it protects.
//
// Sealing the same content twice gives different bytes: the cipher takes a fresh nonce each time.
// So two records that happen to hold the same password do not look alike to whoever is holding the
// database.
//
// An empty payload is refused. A record always carries at least its guid, so an empty one means
// the caller has gone wrong, and sealing nothing would put a record on the router that opens into
// nothing.
bool sealPayload(std::string_view key, std::string_view payload, std::string* out);
bool openPayload(std::string_view key, std::string_view sealed, std::string* out);

} // namespace console

#endif // CONSOLE_BOOK_SYNC_KEY_H
