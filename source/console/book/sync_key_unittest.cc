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

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace console {

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, salt_has_the_expected_size)
{
    EXPECT_EQ(createSyncSalt().size(), kSyncSaltSize);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, salts_differ)
{
    std::set<std::string> salts;
    for (int i = 0; i < 50; ++i)
        salts.insert(createSyncSalt());

    EXPECT_EQ(salts.size(), 50u);
}

//--------------------------------------------------------------------------------------------------
// The point of the whole arrangement: the same passphrase and the same salt give the same key on
// every machine, which is what lets seven people read each other's records.
TEST(sync_key_test, same_passphrase_and_salt_give_the_same_key)
{
    const std::string salt = createSyncSalt();

    const std::string first = deriveSyncKey("department passphrase", salt);
    const std::string second = deriveSyncKey("department passphrase", salt);

    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

//--------------------------------------------------------------------------------------------------
// And the reason the salt has to come from the router rather than from each book: the same
// passphrase with a different salt is a different key.
TEST(sync_key_test, different_salt_gives_a_different_key)
{
    const std::string key_one = deriveSyncKey("department passphrase", createSyncSalt());
    const std::string key_two = deriveSyncKey("department passphrase", createSyncSalt());

    ASSERT_FALSE(key_one.empty());
    ASSERT_FALSE(key_two.empty());
    EXPECT_NE(key_one, key_two);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, different_passphrase_gives_a_different_key)
{
    const std::string salt = createSyncSalt();

    EXPECT_NE(deriveSyncKey("one", salt), deriveSyncKey("another", salt));
}

//--------------------------------------------------------------------------------------------------
// A salt of the wrong size did not come from the router. Deriving from it anyway would produce a
// key that looks usable and is not the one everybody else has.
TEST(sync_key_test, refuses_a_salt_of_the_wrong_size)
{
    EXPECT_TRUE(deriveSyncKey("passphrase", std::string()).empty());
    EXPECT_TRUE(deriveSyncKey("passphrase", std::string(kSyncSaltSize - 1, 'x')).empty());
    EXPECT_TRUE(deriveSyncKey("passphrase", std::string(kSyncSaltSize + 1, 'x')).empty());
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, refuses_an_empty_passphrase)
{
    EXPECT_TRUE(deriveSyncKey(std::string(), createSyncSalt()).empty());
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, sealed_payload_opens_back)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());
    ASSERT_FALSE(key.empty());

    const std::string payload = "a computer record, more or less";

    std::string sealed;
    ASSERT_TRUE(sealPayload(key, payload, &sealed));

    std::string opened;
    ASSERT_TRUE(openPayload(key, sealed, &opened));

    EXPECT_EQ(opened, payload);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, sealed_payload_does_not_hold_the_plaintext)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());
    const std::string payload = "password: hunter2";

    std::string sealed;
    ASSERT_TRUE(sealPayload(key, payload, &sealed));

    EXPECT_EQ(sealed.find("hunter2"), std::string::npos);
}

//--------------------------------------------------------------------------------------------------
// Two records holding the same password must not look alike to whoever holds the database.
TEST(sync_key_test, sealing_twice_gives_different_bytes)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());
    const std::string payload = "the same content";

    std::string first;
    std::string second;
    ASSERT_TRUE(sealPayload(key, payload, &first));
    ASSERT_TRUE(sealPayload(key, payload, &second));

    EXPECT_NE(first, second);
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, another_key_does_not_open_the_payload)
{
    const std::string salt = createSyncSalt();
    const std::string key = deriveSyncKey("right", salt);
    const std::string wrong_key = deriveSyncKey("wrong", salt);

    std::string sealed;
    ASSERT_TRUE(sealPayload(key, "content", &sealed));

    std::string opened;
    EXPECT_FALSE(openPayload(wrong_key, sealed, &opened));
}

//--------------------------------------------------------------------------------------------------
// The cipher authenticates what it opens, so a record altered on the way does not decrypt into
// something else - it does not decrypt at all.
TEST(sync_key_test, altered_payload_does_not_open)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());

    std::string sealed;
    ASSERT_TRUE(sealPayload(key, "content", &sealed));
    ASSERT_FALSE(sealed.empty());

    sealed[sealed.size() / 2] ^= 0x01;

    std::string opened;
    EXPECT_FALSE(openPayload(key, sealed, &opened));
}

//--------------------------------------------------------------------------------------------------
// The cipher underneath refuses an empty buffer, so this does too rather than pretending to have
// sealed something. A record never produces an empty payload - its guid is always in there - so
// this is a guard against a caller that has gone wrong, not a case to support.
TEST(sync_key_test, refuses_an_empty_payload)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());

    std::string sealed;
    EXPECT_FALSE(sealPayload(key, std::string(), &sealed));
}

//--------------------------------------------------------------------------------------------------
// Somebody who mistypes the passphrase has to be told before they seal anything with the wrong
// key and send it to everybody else.
TEST(sync_key_test, verifier_accepts_the_right_key)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());

    const std::string verifier = createKeyVerifier(key);
    ASSERT_FALSE(verifier.empty());

    EXPECT_TRUE(checkKeyVerifier(key, verifier));
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, verifier_rejects_a_wrong_key)
{
    const std::string salt = createSyncSalt();

    const std::string verifier = createKeyVerifier(deriveSyncKey("right", salt));
    ASSERT_FALSE(verifier.empty());

    EXPECT_FALSE(checkKeyVerifier(deriveSyncKey("wrong", salt), verifier));
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, verifier_rejects_the_same_passphrase_under_another_salt)
{
    const std::string verifier = createKeyVerifier(deriveSyncKey("passphrase", createSyncSalt()));

    EXPECT_FALSE(checkKeyVerifier(deriveSyncKey("passphrase", createSyncSalt()), verifier));
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, verifier_rejects_nonsense)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());

    EXPECT_FALSE(checkKeyVerifier(key, std::string()));
    EXPECT_FALSE(checkKeyVerifier(key, "not a verifier"));
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, refuses_to_work_without_a_key)
{
    std::string out;
    EXPECT_FALSE(sealPayload(std::string(), "content", &out));
    EXPECT_FALSE(openPayload(std::string(), "content", &out));
    EXPECT_TRUE(createKeyVerifier(std::string()).empty());
}

//--------------------------------------------------------------------------------------------------
TEST(sync_key_test, tolerates_null_output)
{
    const std::string key = deriveSyncKey("passphrase", createSyncSalt());

    EXPECT_FALSE(sealPayload(key, "content", nullptr));
    EXPECT_FALSE(openPayload(key, "content", nullptr));
}

} // namespace console
