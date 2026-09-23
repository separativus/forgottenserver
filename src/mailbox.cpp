// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "mailbox.h"

#include "depotchest.h"
#include "game.h"
#include "iologindata.h"

extern Game g_game;

namespace {

// Where a letter or parcel lands: the depot of the town on the label's second
// line, the way a 7.x post office read it, or the receiver's home town.
Container* getDeliveryContainer(Player& player, const std::string& townName)
{
	const Town* town = g_game.map.towns.getTown(townName);
	if (!town) {
		town = player.getTown();
	}

	DepotChest* depotChest = player.getDepotChest(town->getDepotId(), true);
	if (!depotChest) {
		return nullptr;
	}
	return depotChest->getDeliveryContainer();
}

} // namespace

ReturnValue Mailbox::queryAdd(int32_t, const Thing& thing, uint32_t, uint32_t, Creature*) const
{
	const Item* item = thing.getItem();
	if (item && Mailbox::canSend(item)) {
		return RETURNVALUE_NOERROR;
	}
	return RETURNVALUE_NOTPOSSIBLE;
}

ReturnValue Mailbox::queryMaxCount(int32_t, const Thing&, uint32_t count, uint32_t& maxQueryCount, uint32_t) const
{
	maxQueryCount = std::max<uint32_t>(1, count);
	return RETURNVALUE_NOERROR;
}

ReturnValue Mailbox::queryRemove(const Thing&, uint32_t, uint32_t, Creature* /*= nullptr */) const
{
	return RETURNVALUE_NOTPOSSIBLE;
}

Cylinder* Mailbox::queryDestination(int32_t&, const Thing&, Item**, uint32_t&) { return this; }

void Mailbox::addThing(Thing* thing) { return addThing(0, thing); }

void Mailbox::addThing(int32_t, Thing* thing)
{
	Item* item = thing->getItem();
	if (item && Mailbox::canSend(item)) {
		sendItem(item);
	}
}

void Mailbox::updateThing(Thing*, uint16_t, uint32_t)
{
	//
}

void Mailbox::replaceThing(uint32_t, Thing*)
{
	//
}

void Mailbox::removeThing(Thing*, uint32_t)
{
	//
}

void Mailbox::postAddNotification(Thing* thing, const Cylinder* oldParent, int32_t index, cylinderlink_t)
{
	getParent()->postAddNotification(thing, oldParent, index, LINK_PARENT);
}

void Mailbox::postRemoveNotification(Thing* thing, const Cylinder* newParent, int32_t index, cylinderlink_t)
{
	getParent()->postRemoveNotification(thing, newParent, index, LINK_PARENT);
}

bool Mailbox::sendItem(Item* item) const
{
	std::string receiver;
	std::string townName;
	if (!getReceiver(item, receiver, townName)) {
		return false;
	}

	/**No need to continue if its still empty**/
	if (receiver.empty()) {
		return false;
	}

	Player* player = g_game.getPlayerByName(receiver);
	if (player) {
		Container* container = getDeliveryContainer(*player, townName);
		if (container && g_game.internalMoveItem(item->getParent(), container, INDEX_WHEREEVER, item,
		                                         item->getItemCount(), nullptr, FLAG_NOLIMIT) == RETURNVALUE_NOERROR) {
			g_game.transformItem(item, item->getID() + 1);
			player->onReceiveMail();
			return true;
		}
	} else {
		Player tmpPlayer(nullptr);
		if (!IOLoginData::loadPlayerByName(&tmpPlayer, receiver)) {
			return false;
		}

		Container* container = getDeliveryContainer(tmpPlayer, townName);
		if (container && g_game.internalMoveItem(item->getParent(), container, INDEX_WHEREEVER, item,
		                                         item->getItemCount(), nullptr, FLAG_NOLIMIT) == RETURNVALUE_NOERROR) {
			g_game.transformItem(item, item->getID() + 1);
			IOLoginData::savePlayer(&tmpPlayer);
			return true;
		}
	}
	return false;
}

bool Mailbox::getReceiver(Item* item, std::string& name, std::string& townName) const
{
	const Container* container = item->getContainer();
	if (container) {
		for (Item* containerItem : container->getItemList()) {
			if (containerItem->getID() == ITEM_LABEL && getReceiver(containerItem, name, townName)) {
				return true;
			}
		}
		return false;
	}

	const std::string& text = item->getText();
	if (text.empty()) {
		return false;
	}

	name = getFirstLine(text);
	boost::algorithm::trim(name);

	if (std::string::size_type newline = text.find('\n'); newline != std::string::npos) {
		townName = getFirstLine(text.substr(newline + 1));
		boost::algorithm::trim(townName);
	}
	return true;
}

bool Mailbox::canSend(const Item* item) { return item->getID() == ITEM_PARCEL || item->getID() == ITEM_LETTER; }
