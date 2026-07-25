function Monster:onDropLoot(corpse)
	if hasEvent.onDropLoot then
		Event.onDropLoot(self, corpse)
	end
	-- no updateKillTracker here: its 0xD1 kill-tracker packet is a Tibia 12
	-- feature — the 7.x client debug-asserts on the unknown packet type
end

function Monster:onSpawn(position, startup, artificial)
	if hasEvent.onSpawn then
		return Event.onSpawn(self, position, startup, artificial)
	end
	return true
end
