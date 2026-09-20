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

#include "console/book/sync_key.h"

#include "base/crypto/data_cryptor_chacha20_poly1305.h"
#include "base/crypto/password_hash.h"
#include "base/crypto/random.h"
#include "base/crypto/secure_memory.h"

namespace console {

namespace {

// What the verifier holds when it is opened with the right key. Its content does not matter; that
// it comes back unchanged does. The cipher authenticates what it opens, so a wrong key fails to
// open the verifier at all rather than producing something that has to be compared.
const char kVerifierPlaintext[] = "aspia address book sync";

} // namespace

//--------------------------------------------------------------------------------------------------
std::string createSyncSalt()
{
    return base::Random::string(kSyncSaltSize);
}

//--------------------------------------------------------------------------------------------------
std::string deriveSyncKey(std::string_view passphrase, std::string_view salt)
{
    // A salt of the wrong size means the caller did not get it from the router, so refusing is the
    // only safe answer: deriving from it anyway would produce a key that looks usable and is not
    // the one everybody else has.
    if (salt.size() != kSyncSaltSize)
        return std::string();

    if (passphrase.empty())
        return std::string();

    return base::PasswordHash::hash(base::PasswordHash::SCRYPT, passphrase, salt);
}

//--------------------------------------------------------------------------------------------------
std::string createKeyVerifier(std::string_view key)
{
    std::string verifier;
    if (!sealPayload(key, kVerifierPlaintext, &verifier))
        return std::string();

    return verifier;
}

//--------------------------------------------------------------------------------------------------
bool checkKeyVerifier(std::string_view key, std::string_view verifier)
{
    if (verifier.empty())
        return false;

    std::string opened;
    if (!openPayload(key, verifier, &opened))
        return false;

    const bool matches = (opened == kVerifierPlaintext);
    base::memZero(&opened);
    return matches;
}

//--------------------------------------------------------------------------------------------------
bool sealPayload(std::string_view key, std::string_view payload, std::string* out)
{
    if (!out || key.empty())
        return false;

    base::DataCryptorChaCha20Poly1305 cryptor(key);
    return cryptor.encrypt(payload, out);
}

//--------------------------------------------------------------------------------------------------
bool openPayload(std::string_view key, std::string_view sealed, std::string* out)
{
    if (!out || key.empty())
        return false;

    base::DataCryptorChaCha20Poly1305 cryptor(key);
    return cryptor.decrypt(sealed, out);
}

} // namespace console
