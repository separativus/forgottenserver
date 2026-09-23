// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "bed.h"

#include "condition.h"
#include "game.h"
#include "iologindata.h"

extern Game g_game;

BedItem::BedItem(uint16_t id) : Item(id) { internalRemoveSleeper(); }

Attr_ReadValue BedItem::readAttr(AttrTypes_t attr, PropStream& propStream)
{
	switch (attr) {
		case ATTR_SLEEPERGUID: {
			uint32_t guid;
			if (!propStream.read<uint32_t>(guid)) {
				return ATTR_READ_ERROR;
			}

			if (guid != 0) {
				std::string name = IOLoginData::getNameByGuid(guid);
				if (!name.empty()) {
					setSpecialDescription(name + " is sleeping there.");
					g_game.setBedSleeper(this, guid);
					sleeperGUID = guid;
				}
			}
			return ATTR_READ_CONTINUE;
		}

		case ATTR_SLEEPSTART: {
			uint32_t sleep_start;
			if (!propStream.read<uint32_t>(sleep_start)) {
				return ATTR_READ_ERROR;
			}

			sleepStart = static_cast<uint64_t>(sleep_start);
			return ATTR_READ_CONTINUE;
		}

		default:
			break;
	}
	return Item::readAttr(attr, propStream);
}

void BedItem::serializeAttr(PropWriteStream& propWriteStream) const
{
	if (sleeperGUID != 0) {
		propWriteStream.write<uint8_t>(ATTR_SLEEPERGUID);
		propWriteStream.write<uint32_t>(sleeperGUID);
	}

	if (sleepStart != 0) {
		propWriteStream.write<uint8_t>(ATTR_SLEEPSTART);
		// FIXME: should be stored as 64-bit, but we need to retain backwards compatibility
		propWriteStream.write<uint32_t>(static_cast<uint32_t>(sleepStart));
	}
}

BedItem* BedItem::getNextBedItem() const
{
	Direction dir = Item::items[id].bedPartnerDir;
	Position targetPos = getNextPosition(dir, getPosition());

	Tile* tile = g_game.map.getTile(targetPos);
	if (!tile) {
		return nullptr;
	}
	return tile->getBedItem();
}

void BedItem::wakeUp(Player* player)
{
	if (!house) {
		return;
	}

	if (sleeperGUID != 0) {
		if (!player) {
			Player regenPlayer(nullptr);
			if (IOLoginData::loadPlayerById(&regenPlayer, sleeperGUID)) {
				regeneratePlayer(&regenPlayer);
				IOLoginData::savePlayer(&regenPlayer);
			}
		} else {
			regeneratePlayer(player);
			g_game.addCreatureHealth(player);
		}
	}

	// update the bedSleepersMap
	g_game.removeBedSleeper(sleeperGUID);

	BedItem* nextBedItem = getNextBedItem();

	// unset sleep info
	internalRemoveSleeper();

	if (nextBedItem) {
		nextBedItem->internalRemoveSleeper();
	}

	// change self and partner's appearance
	updateAppearance(nullptr);

	if (nextBedItem) {
		nextBedItem->updateAppearance(nullptr);
	}
}

void BedItem::regeneratePlayer(Player* player) const
{
	const uint32_t sleptTime = time(nullptr) - sleepStart;

	Condition* condition = player->getCondition(CONDITION_REGENERATION, CONDITIONID_DEFAULT);
	if (condition) {
		uint32_t regen;
		if (condition->getTicks() != -1) {
			regen = std::min<int32_t>((condition->getTicks() / 1000), sleptTime) / 30;
			const int32_t newRegenTicks = condition->getTicks() - (regen * 30000);
			if (newRegenTicks <= 0) {
				player->removeCondition(condition);
			} else {
				condition->setTicks(newRegenTicks);
			}
		} else {
			regen = sleptTime / 30;
		}

		player->changeHealth(regen, false);
		player->changeMana(regen);
	}

	const int32_t soulRegen = sleptTime / (60 * 15);
	player->changeSoul(soulRegen);
}

void BedItem::updateAppearance(const Player* player)
{
	const ItemType& it = Item::items[id];
	if (it.type == ITEM_TYPE_BED) {
		if (player && it.transformToOnUse[player->getSex()] != 0) {
			const ItemType& newType = Item::items[it.transformToOnUse[player->getSex()]];
			if (newType.type == ITEM_TYPE_BED) {
				g_game.transformItem(this, it.transformToOnUse[player->getSex()]);
			}
		} else if (it.transformToFree != 0) {
			const ItemType& newType = Item::items[it.transformToFree];
			if (newType.type == ITEM_TYPE_BED) {
				g_game.transformItem(this, it.transformToFree);
			}
		}
	}
}

void BedItem::internalSetSleeper(const Player* player)
{
	std::string desc_str = player->getName() + " is sleeping there.";

	sleeperGUID = player->getGUID();
	sleepStart = time(nullptr);
	setSpecialDescription(desc_str);
}

void BedItem::internalRemoveSleeper()
{
	sleeperGUID = 0;
	sleepStart = 0;
	setSpecialDescription("Nobody is sleeping there.");
}
