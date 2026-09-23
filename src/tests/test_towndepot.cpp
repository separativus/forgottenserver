#define BOOST_TEST_MODULE towndepot

#include "../otpch.h"

#include "../tools.h"
#include "../town.h"

#include <boost/test/unit_test.hpp>

using tfs::town::depotDistance;
using tfs::town::MapLocker;
using tfs::town::nearestLocker;

BOOST_AUTO_TEST_CASE(test_distance_is_chebyshev_on_a_floor)
{
	BOOST_TEST(depotDistance({100, 100, 7}, {100, 100, 7}) == 0u);
	BOOST_TEST(depotDistance({100, 100, 7}, {104, 101, 7}) == 4u);
	BOOST_TEST(depotDistance({100, 100, 7}, {97, 109, 7}) == 9u);
}

BOOST_AUTO_TEST_CASE(test_every_floor_costs_the_penalty)
{
	BOOST_TEST(depotDistance({100, 100, 7}, {100, 100, 8}) == tfs::town::DEPOT_FLOOR_PENALTY);
	BOOST_TEST(depotDistance({100, 100, 7}, {103, 100, 5}) == 3 + 2 * tfs::town::DEPOT_FLOOR_PENALTY);
}

BOOST_AUTO_TEST_CASE(test_no_lockers_means_no_depot) { BOOST_TEST(nearestLocker({100, 100, 7}, {}) == nullptr); }

BOOST_AUTO_TEST_CASE(test_the_nearest_locker_wins)
{
	// the depot id is the locker's own, not the town's: one map, several worlds
	std::vector<MapLocker> lockers{{{4256, 50, 7}, 1}, {{160, 60, 7}, 7}, {{8350, 58, 7}, 0}};
	BOOST_TEST(nearestLocker({160, 54, 7}, lockers)->depotId == 7);
	BOOST_TEST(nearestLocker({4255, 57, 7}, lockers)->depotId == 1);
	BOOST_TEST(nearestLocker({8351, 57, 7}, lockers)->depotId == 0);
}

BOOST_AUTO_TEST_CASE(test_a_locker_on_the_temple_floor_beats_one_right_below)
{
	std::vector<MapLocker> lockers{{{100, 100, 8}, 1}, {{108, 100, 7}, 2}};
	BOOST_TEST(nearestLocker({100, 100, 7}, lockers)->depotId == 2);
}

BOOST_AUTO_TEST_CASE(test_a_tie_goes_to_the_first_locker_recorded)
{
	std::vector<MapLocker> lockers{{{105, 100, 7}, 3}, {{95, 100, 7}, 4}};
	BOOST_TEST(nearestLocker({100, 100, 7}, lockers)->depotId == 3);
}

BOOST_AUTO_TEST_CASE(test_a_town_without_a_locker_in_reach_borrows_the_nearest)
{
	// resolveTownDepots warns about it at boot, but still takes this locker
	std::vector<MapLocker> lockers{{{431, 260, 7}, 1}, {{4000, 260, 7}, 0}};
	const MapLocker* locker = nearestLocker({472, 167, 7}, lockers);
	BOOST_TEST(locker->depotId == 1);
	BOOST_TEST(depotDistance({472, 167, 7}, locker->position) > tfs::town::DEPOT_REACH);
}
