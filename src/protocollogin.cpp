// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocollogin.h"

#include "ban.h"
#include "configmanager.h"
#include "game.h"
#include "iologindata.h"
#include "outputmessage.h"
#include "tasks.h"

extern Game g_game;

namespace {

uint32_t serverIPBytes()
{
	// The 7.6 charlist carries the gameworld IP as 4 raw bytes.
	boost::system::error_code ec;
	auto address = boost::asio::ip::make_address_v4(getString(ConfigManager::IP), ec);
	if (ec) {
		address = boost::asio::ip::address_v4::loopback();
	}

	auto bytes = address.to_bytes();
	uint32_t ip;
	std::memcpy(&ip, bytes.data(), sizeof(ip));
	return ip;
}

} // namespace

void ProtocolLogin::disconnectClient(const std::string& message)
{
	auto output = OutputMessagePool::getOutputMessage();

	output->addByte(0x0A);
	output->addString(message);
	send(output);

	disconnect();
}

void ProtocolLogin::getCharacterList(uint32_t accountNumber, const std::string& password)
{
	Database& db = Database::getInstance();

	DBResult_ptr result = db.storeQuery(
	    fmt::format("SELECT `id`, UNHEX(`password`) AS `password`, `premium_ends_at` FROM `accounts` WHERE `name` = {:s}",
	                db.escapeString(std::to_string(accountNumber))));
	if (!result) {
		disconnectClient("Account number or password is not correct.");
		return;
	}

	if (transformToSHA1(password) != result->getString("password")) {
		disconnectClient("Account number or password is not correct.");
		return;
	}

	Account account;
	account.id = result->getNumber<uint32_t>("id");
	account.premiumEndsAt = result->getNumber<time_t>("premium_ends_at");

	result = db.storeQuery(fmt::format(
	    "SELECT `name` FROM `players` WHERE `account_id` = {:d} AND `deletion` = 0 ORDER BY `name` ASC", account.id));
	if (result) {
		do {
			account.characters.emplace_back(result->getString("name"));
		} while (result->next());
	}

	auto output = OutputMessagePool::getOutputMessage();

	const std::string& motd = getString(ConfigManager::MOTD);
	if (!motd.empty()) {
		output->addByte(0x14);
		output->addString(fmt::format("{:d}\n{:s}", g_game.getMotdNum(), motd));
	}

	// Add char list
	output->addByte(0x64);

	uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), account.characters.size());
	output->addByte(size);

	uint32_t serverIP = serverIPBytes();
	for (uint8_t i = 0; i < size; i++) {
		output->addString(account.characters[i]);
		output->addString(getString(ConfigManager::SERVER_NAME));
		output->add<uint32_t>(serverIP);
		output->add<uint16_t>(getNumber(ConfigManager::GAME_PORT));
	}

	// Add premium days
	if (getBoolean(ConfigManager::FREE_PREMIUM)) {
		output->add<uint16_t>(0xFFFF); // client displays free premium
	} else {
		output->add<uint16_t>(std::max<time_t>(0, account.premiumEndsAt - time(nullptr)) / 86400);
	}

	send(output);

	disconnect();
}

// Character list request
void ProtocolLogin::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	msg.skipBytes(2); // client OS

	uint16_t version = msg.get<uint16_t>();
	if (version != 760) {
		disconnectClient(fmt::format("Only clients with protocol {:s} allowed!", CLIENT_VERSION_STR));
		return;
	}

	msg.skipBytes(12); // dat, spr, pic signatures (4 bytes each)

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		disconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient("Gameworld is under maintenance.\nPlease re-connect in a while.");
		return;
	}

	auto connection = getConnection();
	if (!connection) {
		return;
	}

	if (const auto& banInfo = IOBan::getIpBanInfo(connection->getIP())) {
		disconnectClient(fmt::format("Your IP has been banned until {:s} by {:s}.\n\nReason specified:\n{:s}",
		                             formatDateShort(banInfo->expiresAt), banInfo->bannedBy, banInfo->reason));
		return;
	}

	uint32_t accountNumber = msg.get<uint32_t>();
	if (accountNumber == 0) {
		disconnectClient("Invalid account number.");
		return;
	}

	auto password = msg.getString();
	if (password.empty()) {
		disconnectClient("Invalid password.");
		return;
	}

	g_dispatcher.addTask([=, thisPtr = std::static_pointer_cast<ProtocolLogin>(shared_from_this()),
	                      password = std::string{password}]() { thisPtr->getCharacterList(accountNumber, password); });
}
