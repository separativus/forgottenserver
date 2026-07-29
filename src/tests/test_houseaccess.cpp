#define BOOST_TEST_MODULE houseaccess

#include "../otpch.h"

#include "../house.h"

#include <boost/test/unit_test.hpp>

using tfs::house::parseStorageToken;

BOOST_AUTO_TEST_CASE(test_parses_a_storage_token)
{
	BOOST_TEST(parseStorageToken("%50001").value() == 50001u);
	BOOST_TEST(parseStorageToken("%0").value() == 0u);
	BOOST_TEST(parseStorageToken("%050001").value() == 50001u);
	BOOST_TEST(parseStorageToken("%4294967295").value() == 4294967295u);
}

BOOST_AUTO_TEST_CASE(test_rejects_lines_that_are_not_tokens)
{
	// Plain player names, guild lines and the everyone marker must stay
	// untouched — parseList only asks about lines starting with '%'.
	BOOST_TEST(!parseStorageToken("bot knight").has_value());
	BOOST_TEST(!parseStorageToken("@donators").has_value());
	BOOST_TEST(!parseStorageToken("*").has_value());
	BOOST_TEST(!parseStorageToken("").has_value());
}

BOOST_AUTO_TEST_CASE(test_rejects_malformed_tokens)
{
	// A malformed token must not fall through to addPlayer(), where it would
	// be looked up as a character name.
	BOOST_TEST(!parseStorageToken("%").has_value());
	BOOST_TEST(!parseStorageToken("%abc").has_value());
	BOOST_TEST(!parseStorageToken("% 5").has_value());
	BOOST_TEST(!parseStorageToken("%5 ").has_value());
	BOOST_TEST(!parseStorageToken("%5x").has_value());
	BOOST_TEST(!parseStorageToken("%-1").has_value());
	BOOST_TEST(!parseStorageToken("%+1").has_value());
	BOOST_TEST(!parseStorageToken("%1.5").has_value());
	BOOST_TEST(!parseStorageToken("%%5").has_value());
}

BOOST_AUTO_TEST_CASE(test_rejects_keys_that_do_not_fit_the_storage_key_type)
{
	// Storage keys are uint32_t; anything wider must be dropped rather than
	// silently wrapping onto an unrelated key.
	BOOST_TEST(!parseStorageToken("%4294967296").has_value());
	BOOST_TEST(!parseStorageToken("%99999999999999999999").has_value());
}
