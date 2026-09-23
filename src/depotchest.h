// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_DEPOTCHEST_H
#define FS_DEPOTCHEST_H

#include "container.h"

class DepotChest final : public Container
{
public:
	explicit DepotChest(uint16_t type, bool paginated = true);

	// serialization
	void setMaxDepotItems(uint32_t maxitems) { maxDepotItems = maxitems; }

	// Mail, a house cleared into the depot and items rescued from the retired
	// inbox must neither fail nor show a 7.x client more things than the chest
	// has slots. This is where the next one goes: the chest while it has a free
	// slot, else the backpack in its first slot while that has one, else a new
	// backpack that takes over the chest's last slot and what lay in it.
	Container* getDeliveryContainer();
	// Depots saved while the chest paginated can hold more than its slots:
	// whatever lies past the last one goes into backpacks the same way.
	void packOverflow();

	// cylinder implementations
	ReturnValue queryAdd(int32_t index, const Thing& thing, uint32_t count, uint32_t flags,
	                     Creature* actor = nullptr) const override;

	void postAddNotification(Thing* thing, const Cylinder* oldParent, int32_t index,
	                         cylinderlink_t link = LINK_OWNER) override;
	void postRemoveNotification(Thing* thing, const Cylinder* newParent, int32_t index,
	                            cylinderlink_t link = LINK_OWNER) override;

	// overrides
	bool canRemove() const override { return false; }

	Cylinder* getParent() const override;
	Cylinder* getRealParent() const override { return parent; }

private:
	uint32_t maxDepotItems = 0;
};

#endif // FS_DEPOTCHEST_H
