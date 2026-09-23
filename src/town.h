// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_TOWN_H
#define FS_TOWN_H

#include "position.h"

class Town
{
public:
	explicit Town(uint32_t id) : id(id) {}

	const Position& getTemplePosition() const { return templePosition; }
	const std::string& getName() const { return name; }

	void setTemplePos(Position pos) { templePosition = pos; }
	void setName(std::string_view name) { this->name = name; }
	uint32_t getID() const { return id; }

	// The depot the town's own locker opens; mail, a lost house and the
	// retired inbox deliver there. IOMap resolves it once the map is loaded.
	uint16_t getDepotId() const { return depotId; }
	void setDepotId(uint16_t depotId) { this->depotId = depotId; }

private:
	uint32_t id;
	std::string name;
	Position templePosition;
	uint16_t depotId = 0;
};

using TownMap = std::map<uint32_t, Town*>;

namespace tfs::town {

// A depot locker as the map places it. Its depot id is the map's
// ATTR_DEPOT_ID, which has nothing to do with the id of the town around it.
struct MapLocker
{
	Position position;
	uint16_t depotId;
};

// Every floor between a temple and a locker costs this many squares.
inline constexpr uint32_t DEPOT_FLOOR_PENALTY = 10;
// A town whose nearest locker is further away than this has none of its own.
inline constexpr uint32_t DEPOT_REACH = 64;

inline uint32_t depotDistance(const Position& temple, const Position& locker)
{
	uint32_t squares = std::max(temple.getDistanceX(locker), temple.getDistanceY(locker));
	uint32_t floors = temple.getDistanceZ(locker);
	return squares + DEPOT_FLOOR_PENALTY * floors;
}

// The locker nearest to a temple, the first one recorded on a tie; nullptr
// when there is none at all.
inline const MapLocker* nearestLocker(const Position& temple, const std::vector<MapLocker>& lockers)
{
	const MapLocker* nearest = nullptr;
	uint32_t nearestDistance = std::numeric_limits<uint32_t>::max();
	for (const MapLocker& locker : lockers) {
		uint32_t distance = depotDistance(temple, locker.position);
		if (distance < nearestDistance) {
			nearest = &locker;
			nearestDistance = distance;
		}
	}
	return nearest;
}

} // namespace tfs::town

class Towns
{
public:
	Towns() = default;
	~Towns()
	{
		for (const auto& it : townMap) {
			delete it.second;
		}
	}

	// non-copyable
	Towns(const Towns&) = delete;
	Towns& operator=(const Towns&) = delete;

	bool addTown(uint32_t townId, Town* town) { return townMap.emplace(townId, town).second; }

	Town* getTown(const std::string& townName) const
	{
		for (const auto& it : townMap) {
			if (caseInsensitiveEqual(townName, it.second->getName())) {
				return it.second;
			}
		}
		return nullptr;
	}

	Town* getTown(uint32_t townId) const
	{
		auto it = townMap.find(townId);
		if (it == townMap.end()) {
			return nullptr;
		}
		return it->second;
	}

	const TownMap& getTowns() const { return townMap; }

private:
	TownMap townMap;
};

#endif // FS_TOWN_H
