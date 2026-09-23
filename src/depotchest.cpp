// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "depotchest.h"

#include "game.h"
#include "tools.h"

extern Game g_game;

DepotChest::DepotChest(uint16_t type, bool paginated /*= true*/) :
    Container{type, items[type].maxItems, true, paginated}
{}

ReturnValue DepotChest::queryAdd(int32_t index, const Thing& thing, uint32_t count, uint32_t flags,
                                 Creature* actor /* = nullptr*/) const
{
	const Item* item = thing.getItem();
	if (!item) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	bool skipLimit = hasBitSet(FLAG_NOLIMIT, flags);
	if (!skipLimit) {
		int32_t addCount = 0;

		if ((item->isStackable() && item->getItemCount() != count)) {
			addCount = 1;
		}

		if (item->getTopParent() != this) {
			if (const Container* container = item->getContainer()) {
				addCount = container->getItemHoldingCount() + 1;
			} else {
				addCount = 1;
			}
		}

		if (getItemHoldingCount() + addCount > maxDepotItems) {
			return RETURNVALUE_DEPOTISFULL;
		}
	}

	return Container::queryAdd(index, thing, count, flags, actor);
}

Container* DepotChest::getDeliveryContainer()
{
	if (hasPagination() || size() < capacity()) {
		return this;
	}

	if (Item* first = getItemByIndex(0); first && first->getID() == ITEM_BACKPACK) {
		Container* backpack = first->getContainer();
		if (backpack->size() < backpack->capacity()) {
			return backpack;
		}
	}

	Item* backpack = Item::CreateItem(ITEM_BACKPACK);
	if (!backpack) {
		return this;
	}

	Item* last = getItemByIndex(size() - 1);
	g_game.internalMoveItem(this, backpack->getContainer(), INDEX_WHEREEVER, last, last->getItemCount(), nullptr,
	                        FLAG_NOLIMIT);
	g_game.internalAddItem(this, backpack, INDEX_WHEREEVER, FLAG_NOLIMIT);
	return backpack->getContainer();
}

void DepotChest::packOverflow()
{
	while (size() > capacity()) {
		Container* container = getDeliveryContainer();
		Item* last = getItemByIndex(size() - 1);
		if (container == this || g_game.internalMoveItem(this, container, INDEX_WHEREEVER, last, last->getItemCount(),
		                                                 nullptr, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
			break;
		}
	}
}

void DepotChest::postAddNotification(Thing* thing, const Cylinder* oldParent, int32_t index, cylinderlink_t)
{
	Cylinder* parent = getParent();
	if (parent) {
		parent->postAddNotification(thing, oldParent, index, LINK_PARENT);
	}
}

void DepotChest::postRemoveNotification(Thing* thing, const Cylinder* newParent, int32_t index, cylinderlink_t)
{
	Cylinder* parent = getParent();
	if (parent) {
		parent->postRemoveNotification(thing, newParent, index, LINK_PARENT);
	}
}

Cylinder* DepotChest::getParent() const
{
	if (parent) {
		return parent->getParent();
	}
	return nullptr;
}
