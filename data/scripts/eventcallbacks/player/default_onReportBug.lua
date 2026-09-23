local ec = EventCallback

ec.onReportBug = function(self, message, position, category)
	if self:getAccountType() == ACCOUNT_TYPE_NORMAL then
		return false
	end

	-- 7.x clients send no map position with a report: record where the reporter stands.
	-- The website lists these reports (volumes/django/apps/feedback, table website_bug_reports).
	local name = self:getName()
	local playerPosition = self:getPosition()
	local text = string.format("[Position: %d, %d, %d] %s", playerPosition.x, playerPosition.y, playerPosition.z, message)
	if not db.query("INSERT INTO `website_bug_reports` (`category`, `character_name`, `email`, `message`, `done`, `created_at`) VALUES ('gameplay', " .. db.escapeString(name) .. ", '', " .. db.escapeString(text) .. ", 0, NOW())") then
		print("[Warning - onReportBug] Could not store the bug report of " .. name .. " in website_bug_reports.")
		self:sendTextMessage(MESSAGE_EVENT_DEFAULT, "There was an error when processing your report, please contact a gamemaster.")
		return true
	end

	self:sendTextMessage(MESSAGE_EVENT_DEFAULT, "Your report has been sent to " .. configManager.getString(configKeys.SERVER_NAME) .. ".")
	return true
end

ec:register()
