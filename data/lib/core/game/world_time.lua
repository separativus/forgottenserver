do
	-- The engine's clock (Game::updateWorldTime): a Tibian day per real hour,
	-- in minutes since Tibian midnight.
	function Game.getWorldTime()
		local now = os.date("*t")
		return math.floor((now.min * 60 + now.sec) / 2.5)
	end

	function Game.getFormattedWorldTime()
		local worldTime = Game.getWorldTime()
		return string.format("%d:%02d", math.floor(worldTime / 60), worldTime % 60)
	end
end
