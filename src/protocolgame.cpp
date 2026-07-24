// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocolgame.h"

#include "ban.h"
#include "condition.h"
#include "configmanager.h"
#include "depotchest.h"
#include "game.h"
#include "inbox.h"
#include "iologindata.h"
#include "iomarket.h"
#include "monster.h"
#include "npc.h"
#include "outfit.h"
#include "outputmessage.h"
#include "player.h"
#include "podium.h"
#include "scheduler.h"
#include "storeinbox.h"

extern CreatureEvents* g_creatureEvents;
extern Chat* g_chat;

namespace {

std::deque<std::pair<int64_t, uint32_t>> waitList; // (timeout, player guid)
auto priorityEnd = waitList.end();

auto findClient(uint32_t guid)
{
	std::size_t slot = 1;
	for (auto it = waitList.begin(), end = waitList.end(); it != end; ++it, ++slot) {
		if (it->second == guid) {
			return std::make_pair(it, slot);
		}
	}
	return std::make_pair(waitList.end(), slot);
}

constexpr int64_t getWaitTime(std::size_t slot)
{
	if (slot < 5) {
		return 5;
	} else if (slot < 10) {
		return 10;
	} else if (slot < 20) {
		return 20;
	} else if (slot < 50) {
		return 60;
	}
	return 120;
}

constexpr int64_t getTimeout(std::size_t slot)
{
	// timeout is set to 15 seconds longer than expected retry attempt
	return getWaitTime(slot) + 15;
}

std::size_t clientLogin(const Player& player)
{
	if (player.hasFlag(PlayerFlag_CanAlwaysLogin) || player.getAccountType() >= ACCOUNT_TYPE_GAMEMASTER) {
		return 0;
	}

	uint32_t maxPlayers = static_cast<uint32_t>(getNumber(ConfigManager::MAX_PLAYERS));
	if (maxPlayers == 0 || (waitList.empty() && g_game.getPlayersOnline() < maxPlayers)) {
		return 0;
	}

	int64_t time = OTSYS_TIME();

	auto it = waitList.begin();
	while (it != waitList.end()) {
		if ((it->first - time) <= 0) {
			it = waitList.erase(it);
		} else {
			++it;
		}
	}

	std::size_t slot;
	std::tie(it, slot) = findClient(player.getGUID());
	if (it != waitList.end()) {
		// If server has capacity for this client, let him in even though his current slot might be higher than 0.
		if ((g_game.getPlayersOnline() + slot) <= maxPlayers) {
			waitList.erase(it);
			return 0;
		}

		// let them wait a bit longer
		it->first = time + (getTimeout(slot) * 1000);
		return slot;
	}

	if (player.isPremium()) {
		priorityEnd = waitList.emplace(priorityEnd, time + (getTimeout(slot + 1) * 1000), player.getGUID());
		return std::distance(waitList.begin(), priorityEnd);
	}

	waitList.emplace_back(time + (getTimeout(waitList.size() + 1) * 1000), player.getGUID());
	return waitList.size();
}

} // namespace

void ProtocolGame::release()
{
	// dispatcher thread
	if (player && player->client == shared_from_this()) {
		player->client.reset();
		player->decrementReferenceCounter();
		player = nullptr;
	}

	OutputMessagePool::getInstance().removeProtocolFromAutosend(shared_from_this());
	Protocol::release();
}

void ProtocolGame::login(uint32_t characterId, uint32_t accountId, OperatingSystem_t operatingSystem)
{
	// dispatcher thread
	Player* foundPlayer = g_game.getPlayerByGUID(characterId);
	if (!foundPlayer || getBoolean(ConfigManager::ALLOW_CLONES)) {
		player = new Player(getThis());

		player->incrementReferenceCounter();
		player->setID();
		player->setGUID(characterId);

		if (!IOLoginData::preloadPlayer(player)) {
			disconnectClient("Your character could not be loaded.");
			return;
		}

		if (IOBan::isPlayerNamelocked(player->getGUID())) {
			disconnectClient("Your character has been namelocked.");
			return;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("The game is just going down.\nPlease try again later.");
			return;
		}

		if (g_game.getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlag_CanAlwaysLogin)) {
			disconnectClient("Server is currently closed.\nPlease try again later.");
			return;
		}

		if (getBoolean(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER &&
		    g_game.getPlayerByAccount(player->getAccount())) {
			disconnectClient("You may only login with one character\nof your account at the same time.");
			return;
		}

		if (!player->hasFlag(PlayerFlag_CannotBeBanned)) {
			if (const auto& banInfo = IOBan::getAccountBanInfo(accountId)) {
				if (banInfo->expiresAt > 0) {
					disconnectClient(
					    fmt::format("Your account has been banned until {:s} by {:s}.\n\nReason specified:\n{:s}",
					                formatDateShort(banInfo->expiresAt), banInfo->bannedBy, banInfo->reason));
				} else {
					disconnectClient(
					    fmt::format("Your account has been permanently banned by {:s}.\n\nReason specified:\n{:s}",
					                banInfo->bannedBy, banInfo->reason));
				}
				return;
			}
		}

		if (std::size_t currentSlot = clientLogin(*player)) {
			uint8_t retryTime = getWaitTime(currentSlot);
			auto output = OutputMessagePool::getOutputMessage();
			output->addByte(0x16);
			output->addString(
			    fmt::format("Too many players online.\nYou are at place {:d} on the waiting list.", currentSlot));
			output->addByte(retryTime);
			send(output);
			disconnect();
			return;
		}

		if (!IOLoginData::loadPlayerById(player, player->getGUID())) {
			disconnectClient("Your character could not be loaded.");
			return;
		}

		player->setOperatingSystem(operatingSystem);

		if (!g_game.placeCreature(player, player->getLoginPosition())) {
			if (!g_game.placeCreature(player, player->getTemplePosition(), false, true)) {
				disconnectClient("Temple position is wrong. Contact the administrator.");
				return;
			}
		}

		if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX) {
			player->registerCreatureEvent("ExtendedOpcode");
		}

		player->lastIP = player->getIP();
		player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
		acceptPackets = true;
	} else {
		if (eventConnect != 0 || !getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN)) {
			// Already trying to connect
			disconnectClient("You are already logged in.");
			return;
		}

		if (foundPlayer->client) {
			foundPlayer->disconnect();
			foundPlayer->isConnecting = true;

			eventConnect = g_scheduler.addEvent(
			    createSchedulerTask(1000, [=, thisPtr = getThis(), playerID = foundPlayer->getID()]() {
				    thisPtr->connect(playerID, operatingSystem);
			    }));
		} else {
			connect(foundPlayer->getID(), operatingSystem);
		}
	}
	OutputMessagePool::getInstance().addProtocolToAutosend(shared_from_this());
}

void ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem)
{
	eventConnect = 0;

	Player* foundPlayer = g_game.getPlayerByID(playerId);
	if (!foundPlayer || foundPlayer->client) {
		disconnectClient("You are already logged in.");
		return;
	}

	if (isConnectionExpired()) {
		// ProtocolGame::release() has been called at this point and the Connection object no longer exists, so we
		// return to prevent leakage of the Player.
		return;
	}

	player = foundPlayer;
	player->incrementReferenceCounter();

	g_chat->removeUserFromAllChannels(*player);
	player->clearModalWindows();
	player->setOperatingSystem(operatingSystem);
	player->isConnecting = false;

	player->client = getThis();
	sendAddCreature(player, player->getPosition(), 0);
	player->lastIP = player->getIP();
	player->lastLoginSaved = std::max<time_t>(time(nullptr), player->lastLoginSaved + 1);
	player->resetIdleTime();
	acceptPackets = true;

	// TibiaFun: reconnecting skips the onLogin creaturescript and the channel
	// removal above dropped the membership, so reopen the combat log window to
	// keep it the default.
	g_game.playerOpenChannel(player->getID(), CHANNEL_LOG);
}

void ProtocolGame::logout(bool displayEffect, bool forced)
{
	// dispatcher thread
	if (!player) {
		return;
	}

	if (!player->isRemoved()) {
		if (!forced) {
			if (!player->isAccessPlayer()) {
				if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
					player->sendCancelMessage(RETURNVALUE_YOUCANNOTLOGOUTHERE);
					return;
				}

				if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
					// TibiaFun #15: scale the logout block with what was fought.
					// Monsters lock briefly (pzLockedMonster), players for the full
					// pzLocked window, and an aggressor (pzLocked flag) for
					// pzLockedAggressor. CONDITION_INFIGHT keeps its vanilla duration
					// so skull/icon mechanics stay untouched.
					int64_t now = OTSYS_TIME();
					bool monsterWindow =
					    now - player->getLastMonsterFight() < getNumber(ConfigManager::PZ_LOCKED_MONSTER);
					bool pvpWindow = now - player->getLastPvpFight() <
					                 getNumber(player->isPzLocked() ? ConfigManager::PZ_LOCKED_AGGRESSOR
					                                                : ConfigManager::PZ_LOCKED);
					if (monsterWindow || pvpWindow) {
						player->sendCancelMessage(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
						return;
					}
				}
			}

			// scripting event - onLogout
			if (!g_creatureEvents->playerLogout(player)) {
				// Let the script handle the error message
				return;
			}
		}

		if (displayEffect && !player->isDead() && !player->isInGhostMode()) {
			g_game.addMagicEffect(player->getPosition(), CONST_ME_POFF);
		}
	}

	disconnect();

	g_game.removeCreature(player);
}

// Login to the game world request
void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	// Server is shutting down
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		disconnect();
		return;
	}

	// Client type and OS used
	OperatingSystem_t operatingSystem = static_cast<OperatingSystem_t>(msg.get<uint16_t>());

	version = msg.get<uint16_t>(); // U16 client version

	// The 7.6 client sends nothing else before the gamemaster flag: no U32
	// version, no dat revision, no RSA/XTEA envelope, no session key. 7.61+
	// wraps the XTEA key and credentials in an RSA envelope.
	if (version < 760) {
		disconnectClient(fmt::format("Only clients with protocol {:s} allowed!", CLIENT_VERSION_STR));
		return;
	}

	if (version > 760) {
		if (!Protocol::RSA_decrypt(msg)) {
			disconnect();
			return;
		}

		xtea::key key;
		key[0] = msg.get<uint32_t>();
		key[1] = msg.get<uint32_t>();
		key[2] = msg.get<uint32_t>();
		key[3] = msg.get<uint32_t>();
		enableXTEAEncryption();
		setXTEAKey(key);
	}

	if (version > CLIENT_VERSION_MAX) {
		disconnectClient(fmt::format("Only clients with protocol {:s} allowed!", CLIENT_VERSION_STR));
		return;
	}

	// Enable extended opcode feature for otclient
	if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX) {
		NetworkMessage opcodeMessage;
		opcodeMessage.addByte(0x32);
		opcodeMessage.addByte(0x00);
		opcodeMessage.add<uint16_t>(0x00);
		writeToOutputBuffer(opcodeMessage);
	}

	msg.skipBytes(1); // Gamemaster flag

	uint32_t accountNumber = msg.get<uint32_t>();
	if (accountNumber == 0) {
		disconnectClient("You must enter your account number.");
		return;
	}

	auto characterName = msg.getString();
	auto password = msg.getString();

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		disconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		disconnectClient("Gameworld is under maintenance. Please re-connect in a while.");
		return;
	}

	auto ip = getIP();
	if (const auto& banInfo = IOBan::getIpBanInfo(ip)) {
		disconnectClient(fmt::format("Your IP has been banned until {:s} by {:s}.\n\nReason specified:\n{:s}",
		                             formatDateShort(banInfo->expiresAt), banInfo->bannedBy, banInfo->reason));
		return;
	}

	uint32_t characterId = 0;
	uint32_t accountId =
	    IOLoginData::gameworldAuthentication(accountNumber, std::string{password}, std::string{characterName}, characterId);
	if (accountId == 0) {
		disconnectClient("Account number or password is not correct.");
		return;
	}

	g_dispatcher.addTask(
	    [=, thisPtr = getThis()]() { thisPtr->login(characterId, accountId, operatingSystem); });
}

void ProtocolGame::disconnectClient(const std::string& message) const
{
	auto output = OutputMessagePool::getOutputMessage();
	output->addByte(0x14);
	output->addString(message);
	send(output);
	disconnect();
}

void ProtocolGame::writeToOutputBuffer(const NetworkMessage& msg)
{
	auto out = getOutputBuffer(msg.getLength());
	out->append(msg);
}

void ProtocolGame::parsePacket(NetworkMessage& msg)
{
	if (!acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN || msg.isEmpty()) {
		return;
	}

	uint8_t recvbyte = msg.getByte();

	if (!player) {
		if (recvbyte == 0x0F) {
			disconnect();
		}

		return;
	}

	// a dead player can not performs actions
	if (player->isRemoved() || player->isDead()) {
		if (recvbyte == 0x0F) {
			disconnect();
			return;
		}

		if (recvbyte != 0x14) {
			return;
		}
	}

	switch (recvbyte) {
		case 0x14:
			g_dispatcher.addTask([thisPtr = getThis()]() { thisPtr->logout(true, false); });
			break;
		case 0x1D:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerReceivePingBack(playerID); });
			break;
		case 0x1E:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerReceivePing(playerID); });
			break;
		// case 0x2A: break; // bestiary tracker
		// case 0x2C: break; // team finder (leader)
		// case 0x2D: break; // team finder (member)
		// case 0x28: break; // stash withdraw
		case 0x32:
			parseExtendedOpcode(msg);
			break; // otclient extended opcode
		case 0x64:
			parseAutoWalk(msg);
			break;
		case 0x65:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_NORTH); });
			break;
		case 0x66:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_EAST); });
			break;
		case 0x67:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_SOUTH); });
			break;
		case 0x68:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_WEST); });
			break;
		case 0x69:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerStopAutoWalk(playerID); });
			break;
		case 0x6A:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_NORTHEAST); });
			break;
		case 0x6B:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_SOUTHEAST); });
			break;
		case 0x6C:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_SOUTHWEST); });
			break;
		case 0x6D:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerMove(playerID, DIRECTION_NORTHWEST); });
			break;
		case 0x6F:
			g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
			                     [playerID = player->getID()]() { g_game.playerTurn(playerID, DIRECTION_NORTH); });
			break;
		case 0x70:
			g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
			                     [playerID = player->getID()]() { g_game.playerTurn(playerID, DIRECTION_EAST); });
			break;
		case 0x71:
			g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
			                     [playerID = player->getID()]() { g_game.playerTurn(playerID, DIRECTION_SOUTH); });
			break;
		case 0x72:
			g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
			                     [playerID = player->getID()]() { g_game.playerTurn(playerID, DIRECTION_WEST); });
			break;
		case 0x77:
			parseEquipObject(msg);
			break;
		case 0x78:
			parseThrow(msg);
			break;
		case 0x79:
			parseLookInShop(msg);
			break;
		case 0x7A:
			parsePlayerPurchase(msg);
			break;
		case 0x7B:
			parsePlayerSale(msg);
			break;
		case 0x7C:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerCloseShop(playerID); });
			break;
		case 0x7D:
			parseRequestTrade(msg);
			break;
		case 0x7E:
			parseLookInTrade(msg);
			break;
		case 0x7F:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerAcceptTrade(playerID); });
			break;
		case 0x80:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerCloseTrade(playerID); });
			break;
		case 0x82:
			parseUseItem(msg);
			break;
		case 0x83:
			parseUseItemEx(msg);
			break;
		case 0x84:
			parseUseWithCreature(msg);
			break;
		case 0x85:
			parseRotateItem(msg);
			break;
		case 0x87:
			parseCloseContainer(msg);
			break;
		case 0x88:
			parseUpArrowContainer(msg);
			break;
		case 0x89:
			parseTextWindow(msg);
			break;
		case 0x8A:
			parseHouseWindow(msg);
			break;
		case 0x8B:
			parseWrapItem(msg);
			break;
		case 0x8C:
			parseLookAt(msg);
			break;
		case 0x8D:
			parseLookInBattleList(msg);
			break;
		case 0x8E: /* join aggression */
			break;
		// case 0x8F: break; // quick loot
		// case 0x90: break; // loot container
		// case 0x91: break; // update loot whitelist
		// case 0x92: break; // request locker items
		case 0x96:
			parseSay(msg);
			break;
		case 0x97:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerRequestChannels(playerID); });
			break;
		case 0x98:
			parseOpenChannel(msg);
			break;
		case 0x99:
			parseCloseChannel(msg);
			break;
		case 0x9A:
			parseOpenPrivateChannel(msg);
			break;
		case 0x9E:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerCloseNpcChannel(playerID); });
			break;
		case 0xA0:
			parseFightModes(msg);
			break;
		case 0xA1:
			parseAttack(msg);
			break;
		case 0xA2:
			parseFollow(msg);
			break;
		case 0xA3:
			parseInviteToParty(msg);
			break;
		case 0xA4:
			parseJoinParty(msg);
			break;
		case 0xA5:
			parseRevokePartyInvite(msg);
			break;
		case 0xA6:
			parsePassPartyLeadership(msg);
			break;
		case 0xA7:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerLeaveParty(playerID); });
			break;
		case 0xA8:
			parseEnableSharedPartyExperience(msg);
			break;
		case 0xAA:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerCreatePrivateChannel(playerID); });
			break;
		case 0xAB:
			parseChannelInvite(msg);
			break;
		case 0xAC:
			parseChannelExclude(msg);
			break;
		// case 0xB1: break; // request highscores
		case 0xBE:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerCancelAttackAndFollow(playerID); });
			break;
		// case 0xC7: break; // request tournament leaderboard
		case 0xC9: /* update tile */
			break;
		case 0xCA:
			parseUpdateContainer(msg);
			break;
		case 0xCC:
			parseSeekInContainer(msg);
			break;
		// case 0xCD: break; // request inspect window
		case 0xD2:
			g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerRequestOutfit(playerID); });
			break;
		case 0xD3:
			parseSetOutfit(msg);
			break;
		// case 0xD5: break; // apply imbuement
		// case 0xD6: break; // clear imbuement
		// case 0xD7: break; // close imbuing window
		case 0xDC:
			parseAddVip(msg);
			break;
		case 0xDD:
			parseRemoveVip(msg);
			break;
		case 0xDE:
			parseEditVip(msg);
			break;
		// case 0xDF: break; // premium shop (?)
		// case 0xE0: break; // premium shop (?)
		// case 0xE4: break; // buy charm rune
		// case 0xE5: break; // request character info (cyclopedia)
		case 0xE6:
			parseBugReport(msg);
			break;
		case 0xE7: /* thank you */
			break;
		case 0xE8:
			parseDebugAssert(msg);
			break;
		// case 0xEF: break; // request store coins transfer
		case 0xF2:
			parseRuleViolationReport(msg);
			break;
		case 0xF3: /* get object info */
			break;

		default:
			// The 7.6 client has no further opcodes; ignore unknown bytes.
			break;
	}

	if (msg.isOverrun()) {
		disconnect();
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile, NetworkMessage& msg)
{
	int32_t count;
	Item* ground = tile->getGround();
	if (ground) {
		msg.addItem(ground);
		count = 1;
	} else {
		count = 0;
	}

	const TileItemVector* items = tile->getItemList();
	if (items) {
		for (auto it = items->getBeginTopItem(), end = items->getEndTopItem(); it != end; ++it) {
			msg.addItem(*it);

			if (++count == MAX_STACKPOS) {
				break;
			}
		}
	}

	const CreatureVector* creatures = tile->getCreatures();
	if (creatures) {
		for (auto it = creatures->rbegin(), end = creatures->rend(); it != end; ++it) {
			const Creature* creature = (*it);
			if (!player->canSeeCreature(creature)) {
				continue;
			}

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(msg, creature, known, removedKnown);
			++count;
		}
	}

	if (items && count < MAX_STACKPOS) {
		for (auto it = items->getBeginDownItem(), end = items->getEndDownItem(); it != end; ++it) {
			msg.addItem(*it);

			if (++count == MAX_STACKPOS) {
				return;
			}
		}
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z, int32_t width, int32_t height,
                                     NetworkMessage& msg)
{
	int32_t skip = -1;
	int32_t startz, endz, zstep;

	if (z > 7) {
		startz = z - 2;
		endz = std::min<int32_t>(MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	} else {
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for (int32_t nz = startz; nz != endz + zstep; nz += zstep) {
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);
	}

	if (skip >= 0) {
		msg.addByte(skip);
		msg.addByte(0xFF);
	}
}

void ProtocolGame::GetFloorDescription(NetworkMessage& msg, int32_t x, int32_t y, int32_t z, int32_t width,
                                       int32_t height, int32_t offset, int32_t& skip)
{
	for (int32_t nx = 0; nx < width; nx++) {
		for (int32_t ny = 0; ny < height; ny++) {
			Tile* tile = g_game.map.getTile(x + nx + offset, y + ny + offset, z);
			if (tile) {
				if (skip >= 0) {
					msg.addByte(skip);
					msg.addByte(0xFF);
				}

				skip = 0;
				GetTileDescription(tile, msg);
			} else if (skip == 0xFE) {
				msg.addByte(0xFF);
				msg.addByte(0xFF);
				skip = -1;
			} else {
				++skip;
			}
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	auto result = knownCreatureSet.insert(id);
	if (!result.second) {
		known = true;
		return;
	}

	known = false;

	if (knownCreatureSet.size() > 1300) {
		// Look for a creature to remove
		for (auto it = knownCreatureSet.begin(), end = knownCreatureSet.end(); it != end; ++it) {
			Creature* creature = g_game.getCreatureByID(*it);
			if (!canSee(creature)) {
				removedKnown = *it;
				knownCreatureSet.erase(it);
				return;
			}
		}

		// Bad situation. Let's just remove anyone.
		auto it = knownCreatureSet.begin();
		if (*it == id) {
			++it;
		}

		removedKnown = *it;
		knownCreatureSet.erase(it);
	} else {
		removedKnown = 0;
	}
}

bool ProtocolGame::canSee(const Creature* c) const
{
	if (!c || !player || c->isRemoved()) {
		return false;
	}

	if (!player->canSeeCreature(c)) {
		return false;
	}

	return canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const { return canSee(pos.x, pos.y, pos.z); }

bool ProtocolGame::canSee(int32_t x, int32_t y, int32_t z) const
{
	if (!player) {
		return false;
	}

	const Position& myPos = player->getPosition();
	if (myPos.z <= 7) {
		// we are on ground level or above (7 -> 0) view is from 7 -> 0
		if (z > 7) {
			return false;
		}
	} else { // if (myPos.z >= 8) { we are underground (8 -> 15) view is +/- 2 from the floor we stand on
		if (std::abs(myPos.getZ() - z) > 2) {
			return false;
		}
	}

	// negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.getZ() - z;
	if ((x >= myPos.getX() - Map::maxClientViewportX + offsetz) &&
	    (x <= myPos.getX() + (Map::maxClientViewportX + 1) + offsetz) &&
	    (y >= myPos.getY() - Map::maxClientViewportY + offsetz) &&
	    (y <= myPos.getY() + (Map::maxClientViewportY + 1) + offsetz)) {
		return true;
	}
	return false;
}

// Parse methods
void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	auto name = msg.getString();
	g_dispatcher.addTask(
	    [playerID = player->getID(), name = std::string{name}]() { g_game.playerChannelInvite(playerID, name); });
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	auto name = msg.getString();
	g_dispatcher.addTask(
	    [=, playerID = player->getID(), name = std::string{name}]() { g_game.playerChannelExclude(playerID, name); });
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelID = msg.get<uint16_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerOpenChannel(playerID, channelID); });
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelID = msg.get<uint16_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerCloseChannel(playerID, channelID); });
}

void ProtocolGame::parseOpenPrivateChannel(NetworkMessage& msg)
{
	auto receiver = msg.getString();
	g_dispatcher.addTask([playerID = player->getID(), receiver = std::string{receiver}]() {
		g_game.playerOpenPrivateChannel(playerID, receiver);
	});
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	uint8_t numdirs = msg.getByte();
	// Plaintext framing (760): the message length covers the 2-byte header
	// and the buffer position starts past it, so both sides line up. XTEA
	// framing (772): the position starts past the inner length word while
	// the length is the inner payload, leaving a fixed offset of 4.
	if (numdirs == 0 || (msg.getBufferPosition() + numdirs) != (msg.getLength() + (version > 760 ? 4 : 0))) {
		return;
	}

	msg.skipBytes(numdirs);

	std::vector<Direction> path;
	path.reserve(numdirs);

	for (uint8_t i = 0; i < numdirs; ++i) {
		uint8_t rawdir = msg.getPreviousByte();
		switch (rawdir) {
			case 1:
				path.push_back(DIRECTION_EAST);
				break;
			case 2:
				path.push_back(DIRECTION_NORTHEAST);
				break;
			case 3:
				path.push_back(DIRECTION_NORTH);
				break;
			case 4:
				path.push_back(DIRECTION_NORTHWEST);
				break;
			case 5:
				path.push_back(DIRECTION_WEST);
				break;
			case 6:
				path.push_back(DIRECTION_SOUTHWEST);
				break;
			case 7:
				path.push_back(DIRECTION_SOUTH);
				break;
			case 8:
				path.push_back(DIRECTION_SOUTHEAST);
				break;
			default:
				break;
		}
	}

	if (path.empty()) {
		return;
	}

	g_dispatcher.addTask(
	    [playerID = player->getID(), path = std::move(path)]() { g_game.playerAutoWalk(playerID, path); });
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	// The 7.6 client sends the lookType as a single byte (7.7 widened it to
	// u16); neither knows addons or mounts.
	Outfit_t newOutfit;
	newOutfit.lookType = version <= 760 ? msg.getByte() : msg.get<uint16_t>();
	newOutfit.lookHead = msg.getByte();
	newOutfit.lookBody = msg.getByte();
	newOutfit.lookLegs = msg.getByte();
	newOutfit.lookFeet = msg.getByte();

	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerChangeOutfit(playerID, newOutfit, false); });
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	uint8_t index = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerUseItem(playerID, pos, stackpos, index, spriteId);
	});
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t fromSpriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.getByte();
	Position toPos = msg.getPosition();
	uint16_t toSpriteId = msg.get<uint16_t>();
	uint8_t toStackPos = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerUseItemEx(playerID, fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId);
	});
}

void ProtocolGame::parseUseWithCreature(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackPos = msg.getByte();
	uint32_t creatureId = msg.get<uint32_t>();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerUseWithCreature(playerID, fromPos, fromStackPos, creatureId, spriteId);
	});
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerCloseContainer(playerID, cid); });
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerMoveUpContainer(playerID, cid); });
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.getByte();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerUpdateContainer(playerID, cid); });
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t fromStackpos = msg.getByte();
	Position toPos = msg.getPosition();
	uint8_t count = msg.getByte();

	if (toPos != fromPos) {
		g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
			g_game.playerMoveThing(playerID, fromPos, spriteId, fromStackpos, toPos, count);
		});
	}
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	msg.skipBytes(2); // spriteId
	uint8_t stackpos = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
	                     [=, playerID = player->getID()]() { g_game.playerLookAt(playerID, pos, stackpos); });
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureID = msg.get<uint32_t>();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
	                     [=, playerID = player->getID()]() { g_game.playerLookInBattleList(playerID, creatureID); });
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	std::string_view receiver;
	uint16_t channelId;

	SpeakClasses type = static_cast<SpeakClasses>(msg.getByte());
	switch (type) {
		case TALKTYPE_PRIVATE:
		case TALKTYPE_PRIVATE_RED:
			receiver = msg.getString();
			channelId = 0;
			break;

		case TALKTYPE_CHANNEL_Y:
		case TALKTYPE_CHANNEL_O:
		case TALKTYPE_CHANNEL_R1:
		case TALKTYPE_CHANNEL_R2:
			channelId = msg.get<uint16_t>();
			break;

		default:
			channelId = 0;
			break;
	}

	auto text = msg.getString();
	if (text.length() > 255) {
		return;
	}

	g_dispatcher.addTask([=, playerID = player->getID(), receiver = std::string{receiver}, text = std::string{text}]() {
		g_game.playerSay(playerID, channelId, type, receiver, text);
	});
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.getByte();  // 1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.getByte();  // 0 - stand while fighting, 1 - chase opponent
	uint8_t rawSecureMode = msg.getByte(); // 0 - can't attack unmarked, 1 - can attack unmarked
	// uint8_t rawPvpMode = msg.getByte(); // pvp mode introduced in 10.0

	fightMode_t fightMode;
	if (rawFightMode == 1) {
		fightMode = FIGHTMODE_ATTACK;
	} else if (rawFightMode == 2) {
		fightMode = FIGHTMODE_BALANCED;
	} else {
		fightMode = FIGHTMODE_DEFENSE;
	}

	g_dispatcher.addTask([=, playerID = player->getID()]() {
		g_game.playerSetFightModes(playerID, fightMode, rawChaseMode != 0, rawSecureMode != 0);
	});
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureID = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureID (same as above)
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerSetAttackedCreature(playerID, creatureID); });
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureID = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureID (same as above)
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerFollowCreature(playerID, creatureID); });
}

void ProtocolGame::parseEquipObject(NetworkMessage& msg)
{
	// hotkey equip (?)
	uint16_t spriteID = msg.get<uint16_t>();
	// msg.get<uint8_t>(); // bool smartMode (?)

	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
	                     [=, playerID = player->getID()]() { g_game.playerEquipItem(playerID, spriteID); });
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextID = msg.get<uint32_t>();
	auto newText = msg.getString();
	g_dispatcher.addTask([playerID = player->getID(), windowTextID, newText]() {
		g_game.playerWriteItem(playerID, windowTextID, newText);
	});
}

void ProtocolGame::parseHouseWindow(NetworkMessage& msg)
{
	uint8_t doorId = msg.getByte();
	uint32_t id = msg.get<uint32_t>();
	auto text = msg.getString();
	g_dispatcher.addTask([=, playerID = player->getID(), text = std::string{text}]() {
		g_game.playerUpdateHouseWindow(playerID, doorId, id, text);
	});
}

void ProtocolGame::parseWrapItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerWrapItem(playerID, pos, stackpos, spriteId);
	});
}

void ProtocolGame::parseLookInShop(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION,
	                     [=, playerID = player->getID()]() { g_game.playerLookInShop(playerID, id, count); });
}

void ProtocolGame::parsePlayerPurchase(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	uint16_t amount = msg.get<uint16_t>();
	bool ignoreCap = msg.getByte() != 0;
	bool inBackpacks = msg.getByte() != 0;
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerPurchaseItem(playerID, id, count, amount, ignoreCap, inBackpacks);
	});
}

void ProtocolGame::parsePlayerSale(NetworkMessage& msg)
{
	uint16_t id = msg.get<uint16_t>();
	uint8_t count = msg.getByte();
	uint16_t amount = msg.get<uint16_t>();
	bool ignoreEquipped = msg.getByte() != 0;
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerSellItem(playerID, id, count, amount, ignoreEquipped);
	});
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	uint32_t playerId = msg.get<uint32_t>();
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerRequestTrade(playerID, pos, stackpos, playerId, spriteId); });
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counterOffer = (msg.getByte() == 0x01);
	uint8_t index = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerLookInTrade(playerID, counterOffer, index);
	});
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	auto name = msg.getString();
	g_dispatcher.addTask(
	    [playerID = player->getID(), name = std::string{name}]() { g_game.playerRequestAddVip(playerID, name); });
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerRequestRemoveVip(playerID, guid); });
}

void ProtocolGame::parseEditVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	auto description = msg.getString();
	uint32_t icon = std::min<uint32_t>(10, msg.get<uint32_t>()); // 10 is max icon in 9.63
	bool notify = msg.getByte() != 0;
	g_dispatcher.addTask([=, playerID = player->getID(), description = std::string{description}]() {
		g_game.playerRequestEditVip(playerID, guid, description, icon, notify);
	});
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	uint8_t stackpos = msg.getByte();
	g_dispatcher.addTask(DISPATCHER_TASK_EXPIRATION, [=, playerID = player->getID()]() {
		g_game.playerRotateItem(playerID, pos, stackpos, spriteId);
	});
}

void ProtocolGame::parseRuleViolationReport(NetworkMessage& msg)
{
	uint8_t reportType = msg.getByte();
	uint8_t reportReason = msg.getByte();
	auto targetName = msg.getString();
	auto comment = msg.getString();
	std::string_view translation;
	if (reportType == REPORT_TYPE_NAME) {
		translation = msg.getString();
	} else if (reportType == REPORT_TYPE_STATEMENT) {
		translation = msg.getString();
		msg.get<uint32_t>(); // statement id, used to get whatever player have said, we don't log that.
	}

	g_dispatcher.addTask([=, playerID = player->getID(), targetName = std::string{targetName},
	                      comment = std::string{comment}, translation = std::string{translation}]() {
		g_game.playerReportRuleViolation(playerID, targetName, reportType, reportReason, comment, translation);
	});
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	uint8_t category = msg.getByte();
	auto message = msg.getString();

	Position position;
	if (category == BUG_CATEGORY_MAP) {
		position = msg.getPosition();
	}

	g_dispatcher.addTask([=, playerID = player->getID(), message = std::string{message}]() {
		g_game.playerReportBug(playerID, message, position, category);
	});
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if (debugAssertSent) {
		return;
	}

	debugAssertSent = true;

	auto assertLine = msg.getString();
	auto date = msg.getString();
	auto description = msg.getString();
	auto comment = msg.getString();
	g_dispatcher.addTask([playerID = player->getID(), assertLine = std::string{assertLine}, date = std::string{date},
	                      description = std::string{description}, comment = std::string{comment}]() {
		g_game.playerDebugAssert(playerID, assertLine, date, description, comment);
	});
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetID = msg.get<uint32_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerInviteToParty(playerID, targetID); });
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetID = msg.get<uint32_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerJoinParty(playerID, targetID); });
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetID = msg.get<uint32_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerRevokePartyInvitation(playerID, targetID); });
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetID = msg.get<uint32_t>();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerPassPartyLeadership(playerID, targetID); });
}

void ProtocolGame::parseEnableSharedPartyExperience(NetworkMessage& msg)
{
	bool sharedExpActive = msg.getByte() == 1;
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerEnableSharedPartyExperience(playerID, sharedExpActive); });
}

void ProtocolGame::parseMarketLeave()
{
	g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerLeaveMarket(playerID); });
}

void ProtocolGame::parseMarketBrowse(NetworkMessage& msg)
{
	uint8_t browseId = msg.get<uint8_t>();
	if (browseId == MARKETREQUEST_OWN_OFFERS) {
		g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerBrowseMarketOwnOffers(playerID); });
	} else if (browseId == MARKETREQUEST_OWN_HISTORY) {
		g_dispatcher.addTask([playerID = player->getID()]() { g_game.playerBrowseMarketOwnHistory(playerID); });
	} else {
		uint16_t spriteID = msg.get<uint16_t>();
		g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerBrowseMarket(playerID, spriteID); });
	}
}

void ProtocolGame::parseMarketCreateOffer(NetworkMessage& msg)
{
	uint8_t type = msg.getByte();
	uint16_t spriteId = msg.get<uint16_t>();

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if (it.id == 0 || it.wareId == 0) {
		return;
	} else if (it.classification > 0) {
		msg.getByte(); // item tier
	}

	uint16_t amount = msg.get<uint16_t>();
	uint64_t price = msg.get<uint64_t>();
	bool anonymous = (msg.getByte() != 0);
	g_dispatcher.addTask([=, playerID = player->getID()]() {
		g_game.playerCreateMarketOffer(playerID, type, spriteId, amount, price, anonymous);
	});
	sendStoreBalance();
}

void ProtocolGame::parseMarketCancelOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerCancelMarketOffer(playerID, timestamp, counter); });
	sendStoreBalance();
}

void ProtocolGame::parseMarketAcceptOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.get<uint32_t>();
	uint16_t counter = msg.get<uint16_t>();
	uint16_t amount = msg.get<uint16_t>();
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerAcceptMarketOffer(playerID, timestamp, counter, amount); });
}

void ProtocolGame::parseModalWindowAnswer(NetworkMessage& msg)
{
	uint32_t id = msg.get<uint32_t>();
	uint8_t button = msg.getByte();
	uint8_t choice = msg.getByte();
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerAnswerModalWindow(playerID, id, button, choice); });
}

void ProtocolGame::parseBrowseField(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	g_dispatcher.addTask([=, playerID = player->getID()]() { g_game.playerBrowseField(playerID, pos); });
}

void ProtocolGame::parseSeekInContainer(NetworkMessage& msg)
{
	uint8_t containerId = msg.getByte();
	uint16_t index = msg.get<uint16_t>();
	g_dispatcher.addTask(
	    [=, playerID = player->getID()]() { g_game.playerSeekInContainer(playerID, containerId, index); });
}

// Send methods
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	NetworkMessage msg;
	msg.addByte(0xAD);
	msg.addString(receiver);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelEvent(uint16_t, const std::string&, ChannelEvent_t)
{
	// 0xF3 channel events are unknown to the 7.6 client.
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x8E);
	msg.add<uint32_t>(creature->getID());
	AddOutfit(msg, outfit);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	AddCreatureLight(msg, creature);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureWalkthrough(const Creature*, bool)
{
	// 0x92 walkthrough updates are unknown to the 7.6 client.
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x91);
	msg.add<uint32_t>(creature->getID());
	msg.addByte(player->getPartyShield(creature->getPlayer()));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if (g_game.getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x90);
	msg.add<uint32_t>(creature->getID());
	msg.addByte(player->getSkullClient(creature));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x86);
	msg.add<uint32_t>(creature->getID());
	msg.addByte(color);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTutorial(uint8_t tutorialId)
{
	NetworkMessage msg;
	msg.addByte(0xDC);
	msg.addByte(tutorialId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc)
{
	NetworkMessage msg;
	msg.addByte(0xDD);
	msg.addPosition(pos);
	msg.addByte(markType);
	msg.addString(desc);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendReLoginWindow(uint8_t unfairFightReduction)
{
	NetworkMessage msg;
	msg.addByte(0x28);
	msg.addByte(0x00);
	msg.addByte(unfairFightReduction);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendStats()
{
	NetworkMessage msg;
	AddPlayerStats(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendExperienceTracker(int64_t, int64_t)
{
	// 0xAF experience tracker is unknown to the 7.6 client.
}

void ProtocolGame::sendTextMessage(const TextMessage& message)
{
	// 7.x text message: type byte + text, nothing else.
	NetworkMessage msg;
	msg.addByte(0xB4);
	msg.addByte(message.type);
	msg.addString(message.text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	NetworkMessage msg;
	msg.addByte(0xB3);
	msg.add<uint16_t>(channelId);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage msg;
	msg.addByte(0xB2);
	msg.add<uint16_t>(channelId);
	msg.addString(channelName);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelsDialog()
{
	NetworkMessage msg;
	msg.addByte(0xAB);

	const ChannelList& list = g_chat->getChannelList(*player);
	msg.addByte(list.size());
	for (ChatChannel* channel : list) {
		msg.add<uint16_t>(channel->getId());
		msg.addString(channel->getName());
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName, const UsersMap*, const InvitedMap*)
{
	// 7.x channel open: id + name, no user/invite lists.
	NetworkMessage msg;
	msg.addByte(0xAC);

	msg.add<uint16_t>(channelId);
	msg.addString(channelName);

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChannelMessage(const std::string& author, const std::string& text, SpeakClasses type,
                                      uint16_t channel)
{
	// No speaker level on the 7.x wire; the u32 statement id only for 7.7+.
	NetworkMessage msg;
	msg.addByte(0xAA);
	if (version > 760) {
		msg.add<uint32_t>(0x00);
	}
	msg.addString(author);
	msg.addByte(type);
	msg.add<uint16_t>(channel);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendIcons(uint32_t icons)
{
	// The 7.6 client reads the condition icons as a single byte.
	NetworkMessage msg;
	msg.addByte(0xA2);
	msg.addByte(static_cast<uint8_t>(std::min<uint32_t>(icons, std::numeric_limits<uint8_t>::max())));
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendContainer(uint8_t cid, const Container* container, bool hasParent, uint16_t firstIndex)
{
	// 7.x container: item, name, capacity, has-parent, item count + items.
	NetworkMessage msg;
	msg.addByte(0x6E);

	msg.addByte(cid);

	msg.addItem(container);
	msg.addString(container->getName());

	msg.addByte(container->capacity());
	msg.addByte(hasParent ? 0x01 : 0x00);

	msg.addByte(std::min<uint32_t>(0xFF, container->size()));

	uint32_t i = 0;
	const ItemDeque& itemList = container->getItemList();
	for (auto it = itemList.begin() + firstIndex, end = itemList.end(); i < 0xFF && it != end; ++it, ++i) {
		msg.addItem(*it);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendEmptyContainer(uint8_t cid)
{
	NetworkMessage msg;
	msg.addByte(0x6E);

	msg.addByte(cid);

	msg.addItem(ITEM_BAG, 1);
	msg.addString("Placeholder");

	msg.addByte(8);
	msg.addByte(0x00);
	msg.addByte(0x00);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendShop(Npc* npc, const ShopInfoList& itemList)
{
	NetworkMessage msg;
	msg.addByte(0x7A);
	msg.addString(npc->getName());

	uint16_t itemsToSend = std::min<size_t>(itemList.size(), std::numeric_limits<uint16_t>::max());
	msg.add<uint16_t>(itemsToSend);

	uint16_t i = 0;
	for (auto it = itemList.begin(); i < itemsToSend; ++it, ++i) {
		AddShopItem(msg, *it);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseShop()
{
	NetworkMessage msg;
	msg.addByte(0x7C);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSaleItemList(const std::list<ShopInfo>& shop)
{
	uint64_t playerBank = player->getBankBalance();
	uint64_t playerMoney = player->getMoney();
	sendResourceBalance(RESOURCE_BANK_BALANCE, playerBank);
	sendResourceBalance(RESOURCE_GOLD_EQUIPPED, playerMoney);

	NetworkMessage msg;
	msg.addByte(0x7B);

	std::map<uint16_t, uint32_t> saleMap;

	if (shop.size() <= 5) {
		// For very small shops it's not worth it to create the complete map
		for (const ShopInfo& shopInfo : shop) {
			if (shopInfo.sellPrice == 0) {
				continue;
			}

			int8_t subtype = -1;

			const ItemType& itemType = Item::items[shopInfo.itemId];
			if (itemType.hasSubType() && !itemType.stackable) {
				subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
			}

			uint32_t count = player->getItemTypeCount(shopInfo.itemId, subtype);
			if (count > 0) {
				saleMap[shopInfo.itemId] = count;
			}
		}
	} else {
		// Large shop, it's better to get a cached map of all item counts and use it We need a temporary map since the
		// finished map should only contain items available in the shop
		std::map<uint32_t, uint32_t> tempSaleMap;
		player->getAllItemTypeCount(tempSaleMap);

		// We must still check manually for the special items that require subtype matches (That is, fluids such as
		// potions etc., actually these items are very few since health potions now use their own ID)
		for (const ShopInfo& shopInfo : shop) {
			if (shopInfo.sellPrice == 0) {
				continue;
			}

			int8_t subtype = -1;

			const ItemType& itemType = Item::items[shopInfo.itemId];
			if (itemType.hasSubType() && !itemType.stackable) {
				subtype = (shopInfo.subType == 0 ? -1 : shopInfo.subType);
			}

			if (subtype != -1) {
				uint32_t count;
				if (itemType.isFluidContainer() || itemType.isSplash()) {
					count = player->getItemTypeCount(shopInfo.itemId,
					                                 subtype); // This shop item requires extra checks
				} else {
					count = subtype;
				}

				if (count > 0) {
					saleMap[shopInfo.itemId] = count;
				}
			} else {
				std::map<uint32_t, uint32_t>::const_iterator findIt = tempSaleMap.find(shopInfo.itemId);
				if (findIt != tempSaleMap.end() && findIt->second > 0) {
					saleMap[shopInfo.itemId] = findIt->second;
				}
			}
		}
	}

	uint8_t itemsToSend = std::min<size_t>(saleMap.size(), std::numeric_limits<uint8_t>::max());
	msg.addByte(itemsToSend);

	uint8_t i = 0;
	for (std::map<uint16_t, uint32_t>::const_iterator it = saleMap.begin(); i < itemsToSend; ++it, ++i) {
		msg.addItemId(it->first);
		msg.add<uint16_t>(std::min<uint16_t>(it->second, std::numeric_limits<uint16_t>::max()));
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendResourceBalance(const ResourceTypes_t, uint64_t)
{
	// 0xEE resource balances are unknown to the 7.6 client.
}

void ProtocolGame::sendStoreBalance()
{
	// 0xDF store balance is unknown to the 7.6 client.
}

void ProtocolGame::sendMarketEnter()
{
	NetworkMessage msg;
	msg.addByte(0xF6);
	msg.addByte(
	    std::min<uint32_t>(IOMarket::getPlayerOfferCount(player->getGUID()), std::numeric_limits<uint8_t>::max()));

	player->setInMarket(true);

	std::map<uint16_t, uint32_t> depotItems;
	std::forward_list<Container*> containerList{player->getInbox()};

	for (const auto& chest : player->depotChests) {
		if (!chest.second->empty()) {
			containerList.push_front(chest.second);
		}
	}

	do {
		Container* container = containerList.front();
		containerList.pop_front();

		for (Item* item : container->getItemList()) {
			Container* c = item->getContainer();
			if (c && !c->empty()) {
				containerList.push_front(c);
				continue;
			}

			const ItemType& itemType = Item::items[item->getID()];
			if (itemType.wareId == 0) {
				continue;
			}

			if (c && (!itemType.isContainer() || c->capacity() != itemType.maxItems)) {
				continue;
			}

			if (!item->hasMarketAttributes()) {
				continue;
			}

			depotItems[itemType.id] += Item::countByType(item, -1);
		}
	} while (!containerList.empty());

	uint16_t itemsToSend = std::min<size_t>(depotItems.size(), std::numeric_limits<uint16_t>::max());
	uint16_t i = 0;

	msg.add<uint16_t>(itemsToSend);
	for (std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(); i < itemsToSend; ++it, ++i) {
		const ItemType& itemType = Item::items[it->first];
		msg.add<uint16_t>(itemType.wareId);
		if (itemType.classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(std::min<uint32_t>(0xFFFF, it->second));
	}
	writeToOutputBuffer(msg);

	sendResourceBalance(RESOURCE_BANK_BALANCE, player->getBankBalance());
	sendResourceBalance(RESOURCE_GOLD_EQUIPPED, player->getMoney());
	sendStoreBalance();
}

void ProtocolGame::sendMarketLeave()
{
	NetworkMessage msg;
	msg.addByte(0xF7);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers,
                                        const MarketOfferList& sellOffers)
{
	sendStoreBalance();

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addByte(MARKETREQUEST_ITEM);
	msg.addItemId(itemId);

	if (Item::items[itemId].classification > 0) {
		msg.addByte(0); // item tier
	}

	msg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
		msg.addString(offer.playerName);
	}

	msg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
		msg.addString(offer.playerName);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketAcceptOffer(const MarketOfferEx& offer)
{
	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addByte(MARKETREQUEST_ITEM);
	msg.addItemId(offer.itemId);
	if (Item::items[offer.itemId].classification > 0) {
		msg.addByte(0);
	}

	if (offer.type == MARKETACTION_BUY) {
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
		msg.addString(offer.playerName);
		msg.add<uint32_t>(0x00);
	} else {
		msg.add<uint32_t>(0x00);
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
		msg.addString(offer.playerName);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addByte(MARKETREQUEST_OWN_OFFERS);

	msg.add<uint32_t>(buyOffers.size());
	for (const MarketOffer& offer : buyOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		if (Item::items[offer.itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
	}

	msg.add<uint32_t>(sellOffers.size());
	for (const MarketOffer& offer : sellOffers) {
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		if (Item::items[offer.itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketCancelOffer(const MarketOfferEx& offer)
{
	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addByte(MARKETREQUEST_OWN_OFFERS);

	if (offer.type == MARKETACTION_BUY) {
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		if (Item::items[offer.itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
		msg.add<uint32_t>(0x00);
	} else {
		msg.add<uint32_t>(0x00);
		msg.add<uint32_t>(0x01);
		msg.add<uint32_t>(offer.timestamp);
		msg.add<uint16_t>(offer.counter);
		msg.addItemId(offer.itemId);
		if (Item::items[offer.itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(offer.amount);
		msg.add<uint64_t>(offer.price);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers,
                                              const HistoryMarketOfferList& sellOffers)
{
	uint32_t i = 0;
	std::map<uint32_t, uint16_t> counterMap;
	uint32_t buyOffersToSend =
	    std::min<uint32_t>(buyOffers.size(), 810 + std::max<int32_t>(0, 810 - sellOffers.size()));
	uint32_t sellOffersToSend =
	    std::min<uint32_t>(sellOffers.size(), 810 + std::max<int32_t>(0, 810 - buyOffers.size()));

	NetworkMessage msg;
	msg.addByte(0xF9);
	msg.addByte(MARKETREQUEST_OWN_HISTORY);

	msg.add<uint32_t>(buyOffersToSend);
	for (auto it = buyOffers.begin(); i < buyOffersToSend; ++it, ++i) {
		msg.add<uint32_t>(it->timestamp);
		msg.add<uint16_t>(counterMap[it->timestamp]++);
		msg.addItemId(it->itemId);
		if (Item::items[it->itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(it->amount);
		msg.add<uint64_t>(it->price);
		msg.addByte(it->state);
	}

	counterMap.clear();
	i = 0;

	msg.add<uint32_t>(sellOffersToSend);
	for (auto it = sellOffers.begin(); i < sellOffersToSend; ++it, ++i) {
		msg.add<uint32_t>(it->timestamp);
		msg.add<uint16_t>(counterMap[it->timestamp]++);
		msg.addItemId(it->itemId);
		if (Item::items[it->itemId].classification > 0) {
			msg.addByte(0);
		}
		msg.add<uint16_t>(it->amount);
		msg.add<uint64_t>(it->price);
		msg.addByte(it->state);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTradeItemRequest(const std::string& traderName, const Item* item, bool ack)
{
	NetworkMessage msg;

	if (ack) {
		msg.addByte(0x7D);
	} else {
		msg.addByte(0x7E);
	}

	msg.addString(traderName);

	if (const Container* tradeContainer = item->getContainer()) {
		std::list<const Container*> listContainer{tradeContainer};
		std::list<const Item*> itemList{tradeContainer};
		while (!listContainer.empty()) {
			const Container* container = listContainer.front();
			listContainer.pop_front();

			for (Item* containerItem : container->getItemList()) {
				Container* tmpContainer = containerItem->getContainer();
				if (tmpContainer) {
					listContainer.push_back(tmpContainer);
				}
				itemList.push_back(containerItem);
			}
		}

		msg.addByte(itemList.size());
		for (const Item* listItem : itemList) {
			msg.addItem(listItem);
		}
	} else {
		msg.addByte(0x01);
		msg.addItem(item);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseTrade()
{
	NetworkMessage msg;
	msg.addByte(0x7F);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCloseContainer(uint8_t cid)
{
	NetworkMessage msg;
	msg.addByte(0x6F);
	msg.addByte(cid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackpos)
{
	if (!canSee(creature)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x6B);
	if (stackpos >= MAX_STACKPOS) {
		msg.add<uint16_t>(0xFFFF);
		msg.add<uint32_t>(creature->getID());
	} else {
		msg.addPosition(creature->getPosition());
		msg.addByte(stackpos);
	}

	msg.add<uint16_t>(0x63);
	msg.add<uint32_t>(creature->getID());
	msg.addByte(creature->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text,
                                   const Position* pos /* = nullptr*/)
{
	// The u32 statement id arrived with the 7.7 client; 7.6 expects the
	// speaker name right away and desyncs on the extra dword.
	NetworkMessage msg;
	msg.addByte(0xAA);

	static uint32_t statementId = 0;
	if (version > 760) {
		msg.add<uint32_t>(++statementId);
	}

	msg.addString(creature->getName());

	msg.addByte(type);
	if (pos) {
		msg.addPosition(*pos);
	} else {
		msg.addPosition(creature->getPosition());
	}

	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text,
                                 uint16_t channelId)
{
	NetworkMessage msg;
	msg.addByte(0xAA);

	static uint32_t statementId = 0;
	if (version > 760) {
		msg.add<uint32_t>(++statementId);
	}

	if (!creature) {
		msg.addString("");
	} else {
		msg.addString(creature->getName());
	}

	msg.addByte(type);
	msg.add<uint16_t>(channelId);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendLogMessage(const std::string& text)
{
	// TibiaFun: authorless line in the "Log" channel window (an empty speaker
	// name renders as plain text in the client console). CHANNEL_O is the
	// channel-orange talk type; TALKTYPE_CHANNEL_Y (yellow) and _R1 (red)
	// stay available for events that should stand out.
	NetworkMessage msg;
	msg.addByte(0xAA);
	static uint32_t statementId = 0;
	if (version > 760) {
		msg.add<uint32_t>(++statementId);
	}
	msg.addString("");
	msg.addByte(TALKTYPE_CHANNEL_O);
	msg.add<uint16_t>(CHANNEL_LOG);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPrivateMessageFrom(const Creature* speaker, const std::string& text)
{
	// TibiaFun NPC conversations: make sure the client has a private chat window
	// for this npc (0xAD opens and focuses it), then deliver the line into it.
	sendOpenPrivateChannel(speaker->getName());

	NetworkMessage msg;
	msg.addByte(0xAA);
	static uint32_t statementId = 0;
	if (version > 760) {
		msg.add<uint32_t>(++statementId);
	}
	msg.addString(speaker->getName());
	msg.addByte(TALKTYPE_PRIVATE);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPrivateMessage(const Player* speaker, SpeakClasses type, const std::string& text)
{
	NetworkMessage msg;
	msg.addByte(0xAA);
	static uint32_t statementId = 0;
	if (version > 760) {
		msg.add<uint32_t>(++statementId);
	}
	if (speaker) {
		msg.addString(speaker->getName());
	} else {
		msg.addString("");
	}
	msg.addByte(type);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelTarget()
{
	NetworkMessage msg;
	msg.addByte(0xA3);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	NetworkMessage msg;
	msg.addByte(0x8F);
	msg.add<uint32_t>(creature->getID());
	msg.add<uint16_t>(speed);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCancelWalk()
{
	NetworkMessage msg;
	msg.addByte(0xB5);
	msg.addByte(player->getDirection());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendSkills()
{
	NetworkMessage msg;
	AddPlayerSkills(msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPing()
{
	NetworkMessage msg;
	msg.addByte(0x1E);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPingBack()
{
	NetworkMessage msg;
	msg.addByte(0x1E);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	// 7.x distance effect: from, to, effect byte (no 13.x effect loop).
	NetworkMessage msg;
	msg.addByte(0x85);
	msg.addPosition(from);
	msg.addPosition(to);
	msg.addByte(type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if (!canSee(pos)) {
		return;
	}

	// 7.x magic effect: position + effect byte (no 13.x effect loop).
	NetworkMessage msg;
	msg.addByte(0x83);
	msg.addPosition(pos);
	msg.addByte(type);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendColoredText(const ColoredText& coloredText)
{
	if (!canSee(coloredText.position)) {
		return;
	}

	// 0x84 animated text: the 7.x way to show damage/heal/exp numbers.
	NetworkMessage msg;
	msg.addByte(0x84);
	msg.addPosition(coloredText.position);
	msg.addByte(coloredText.color);
	msg.addString(coloredText.text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendWorldLight(LightInfo lightInfo)
{
	NetworkMessage msg;
	AddWorldLight(msg, lightInfo);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	NetworkMessage msg;
	msg.addByte(0x8C);
	msg.add<uint32_t>(creature->getID());

	if (creature->isHealthHidden()) {
		msg.addByte(0x00);
	} else {
		msg.addByte(std::ceil(
		    (static_cast<double>(creature->getHealth()) / std::max<int32_t>(creature->getMaxHealth(), 1)) * 100));
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendFYIBox(const std::string& message)
{
	NetworkMessage msg;
	msg.addByte(0x15);
	msg.addString(message);
	writeToOutputBuffer(msg);
}

// tile
void ProtocolGame::sendMapDescription(const Position& pos)
{
	NetworkMessage msg;
	msg.addByte(0x64);
	msg.addPosition(player->getPosition());
	GetMapDescription(pos.x - Map::maxClientViewportX, pos.y - Map::maxClientViewportY, pos.z,
	                  (Map::maxClientViewportX * 2) + 2, (Map::maxClientViewportY * 2) + 2, msg);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendAddTileItem(const Position& pos, uint32_t, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	// 7.x 0x6A carries no stackpos byte.
	NetworkMessage msg;
	msg.addByte(0x6A);
	msg.addPosition(pos);
	msg.addItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x6B);
	msg.addPosition(pos);
	msg.addByte(stackpos);
	msg.addItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveTileThing(const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	RemoveTileThing(msg, pos, stackpos);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTileCreature(const Position& pos, uint32_t stackpos, const Creature* creature)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x6B);
	msg.addPosition(pos);
	msg.addByte(stackpos);

	bool known;
	uint32_t removedKnown;
	checkCreatureAsKnown(creature->getID(), known, removedKnown);
	AddCreature(msg, creature, false, removedKnown);

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveTileCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{
	if (stackpos < MAX_STACKPOS) {
		if (!canSee(pos)) {
			return;
		}

		NetworkMessage msg;
		RemoveTileThing(msg, pos, stackpos);
		writeToOutputBuffer(msg);
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x6C);
	msg.add<uint16_t>(0xFFFF);
	msg.add<uint32_t>(creature->getID());
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if (!canSee(pos)) {
		return;
	}

	NetworkMessage msg;
	msg.addByte(0x69);
	msg.addPosition(pos);

	if (tile) {
		GetTileDescription(tile, msg);
		msg.addByte(0x00);
		msg.addByte(0xFF);
	} else {
		msg.addByte(0x01);
		msg.addByte(0xFF);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateCreatureIcons(const Creature*)
{
	// 0x8B creature icons are unknown to the 7.6 client.
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, int32_t stackpos,
                                   MagicEffectClasses magicEffect /*= CONST_ME_NONE*/)
{
	if (!canSee(pos)) {
		return;
	}

	if (creature != player) {
		// stack pos is always real index now, so it can exceed the limit if stack pos exceeds the limit, we need to
		// refresh the tile instead
		// 1. this is a rare case, and is only triggered by forcing summon in a position
		// 2. since no stackpos will be send to the client about that creature, removing it must be done with its id if
		// its stackpos remains >= MAX_STACKPOS. this is done to add creatures to battle list instead of rendering on
		// screen
		if (stackpos >= MAX_STACKPOS) {
			// @todo: should we avoid this check?
			if (const Tile* tile = creature->getTile()) {
				sendUpdateTile(tile, pos);
			}
		} else {
			// 7.x 0x6A carries no stackpos byte.
			NetworkMessage msg;
			msg.addByte(0x6A);
			msg.addPosition(pos);

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(creature->getID(), known, removedKnown);
			AddCreature(msg, creature, known, removedKnown);
			writeToOutputBuffer(msg);
		}

		if (magicEffect != CONST_ME_NONE) {
			sendMagicEffect(pos, magicEffect);
		}
		return;
	}

	// 7.x self-appear packet
	NetworkMessage msg;
	msg.addByte(0x0A);

	msg.add<uint32_t>(player->getID());
	msg.addByte(0x32); // beat duration (50)
	msg.addByte(0x00);

	// can report bugs?
	msg.addByte(player->getAccountType() >= ACCOUNT_TYPE_TUTOR ? 0x01 : 0x00);

	if (player->getAccountType() >= ACCOUNT_TYPE_GAMEMASTER) {
		msg.addByte(0x0B);
		for (uint8_t i = 0; i < 32; i++) {
			msg.addByte(0xFF);
		}
	}

	writeToOutputBuffer(msg);

	sendMapDescription(pos);

	// send login effect
	if (magicEffect != CONST_ME_NONE) {
		sendMagicEffect(pos, magicEffect);
	}

	// send equipment
	for (int i = CONST_SLOT_FIRST; i <= CONST_SLOT_LAST; ++i) {
		sendInventoryItem(static_cast<slots_t>(i), player->getInventoryItem(static_cast<slots_t>(i)));
	}

	sendStats();
	sendSkills();

	// gameworld light-settings
	sendWorldLight(g_game.getWorldLightInfo());

	// player light level
	sendCreatureLight(creature);

	// player vip list
	sendVIPEntries();

	player->sendIcons(); // active conditions
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Position& newPos, int32_t newStackPos,
                                    const Position& oldPos, int32_t oldStackPos, bool teleport)
{
	if (creature == player) {
		if (teleport) {
			sendRemoveTileCreature(creature, oldPos, oldStackPos);
			sendMapDescription(newPos);
		} else {
			NetworkMessage msg;
			if (oldPos.z == 7 && newPos.z >= 8) {
				RemoveTileCreature(msg, creature, oldPos, oldStackPos);
			} else {
				msg.addByte(0x6D);
				if (oldStackPos < MAX_STACKPOS) {
					msg.addPosition(oldPos);
					msg.addByte(oldStackPos);
				} else {
					msg.add<uint16_t>(0xFFFF);
					msg.add<uint32_t>(creature->getID());
				}
				msg.addPosition(newPos);
			}

			if (newPos.z > oldPos.z) {
				MoveDownCreature(msg, creature, newPos, oldPos);
			} else if (newPos.z < oldPos.z) {
				MoveUpCreature(msg, creature, newPos, oldPos);
			}

			if (oldPos.y > newPos.y) { // north, for old x
				msg.addByte(0x65);
				GetMapDescription(oldPos.x - Map::maxClientViewportX, newPos.y - Map::maxClientViewportY, newPos.z,
				                  (Map::maxClientViewportX * 2) + 2, 1, msg);
			} else if (oldPos.y < newPos.y) { // south, for old x
				msg.addByte(0x67);
				GetMapDescription(oldPos.x - Map::maxClientViewportX, newPos.y + (Map::maxClientViewportY + 1),
				                  newPos.z, (Map::maxClientViewportX * 2) + 2, 1, msg);
			}

			if (oldPos.x < newPos.x) { // east, [with new y]
				msg.addByte(0x66);
				GetMapDescription(newPos.x + (Map::maxClientViewportX + 1), newPos.y - Map::maxClientViewportY,
				                  newPos.z, 1, (Map::maxClientViewportY * 2) + 2, msg);
			} else if (oldPos.x > newPos.x) { // west, [with new y]
				msg.addByte(0x68);
				GetMapDescription(newPos.x - Map::maxClientViewportX, newPos.y - Map::maxClientViewportY, newPos.z, 1,
				                  (Map::maxClientViewportY * 2) + 2, msg);
			}
			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos) && canSee(creature->getPosition())) {
		if (teleport || (oldPos.z == 7 && newPos.z >= 8)) {
			sendRemoveTileCreature(creature, oldPos, oldStackPos);
			sendAddCreature(creature, newPos, newStackPos);
		} else {
			NetworkMessage msg;
			msg.addByte(0x6D);
			if (oldStackPos < MAX_STACKPOS) {
				msg.addPosition(oldPos);
				msg.addByte(oldStackPos);
			} else {
				msg.add<uint16_t>(0xFFFF);
				msg.add<uint32_t>(creature->getID());
			}
			msg.addPosition(creature->getPosition());
			writeToOutputBuffer(msg);
		}
	} else if (canSee(oldPos)) {
		sendRemoveTileCreature(creature, oldPos, oldStackPos);
	} else if (canSee(creature->getPosition())) {
		sendAddCreature(creature, newPos, newStackPos);
	}
}

void ProtocolGame::sendInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage msg;
	if (item) {
		msg.addByte(0x78);
		msg.addByte(slot);
		msg.addItem(item);
	} else {
		msg.addByte(0x79);
		msg.addByte(slot);
	}
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendItems()
{
	// 0xF5 action-bar item list is unknown to the 7.6 client.
}

void ProtocolGame::sendAddContainerItem(uint8_t cid, uint16_t, const Item* item)
{
	// 7.x adds container items to the front; no slot on the wire.
	NetworkMessage msg;
	msg.addByte(0x70);
	msg.addByte(cid);
	msg.addItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint16_t slot, const Item* item)
{
	NetworkMessage msg;
	msg.addByte(0x71);
	msg.addByte(cid);
	msg.addByte(slot);
	msg.addItem(item);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint16_t slot, const Item*)
{
	NetworkMessage msg;
	msg.addByte(0x72);
	msg.addByte(cid);
	msg.addByte(slot);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite)
{
	NetworkMessage msg;
	msg.addByte(0x96);
	msg.add<uint32_t>(windowTextId);
	msg.addItem(item);

	if (canWrite) {
		msg.add<uint16_t>(maxlen);
		msg.addString(item->getText());
	} else {
		const std::string& text = item->getText();
		msg.add<uint16_t>(text.size());
		msg.addString(text);
	}

	// 7.x text window: writer name only — no "(traded)" flag, no date.
	msg.addString(item->getWriter());

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	NetworkMessage msg;
	msg.addByte(0x96);
	msg.add<uint32_t>(windowTextId);
	msg.addItem(itemId, 1);
	msg.add<uint16_t>(text.size());
	msg.addString(text);
	msg.addString(""); // writer name
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, const std::string& text)
{
	NetworkMessage msg;
	msg.addByte(0x97);
	msg.addByte(0x00);
	msg.add<uint32_t>(windowTextId);
	msg.addString(text);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendCombatAnalyzer(CombatType_t, int32_t, DamageAnalyzerImpactType, const std::string&)
{
	// 0xCC combat analyzer is unknown to the 7.6 client.
}

void ProtocolGame::sendOutfitWindow()
{
	// 7.x outfit picker: current outfit + a first/last outfit id range —
	// single-byte ids for the 7.6 client, u16 for 7.7+ (see AddOutfit).
	NetworkMessage msg;
	msg.addByte(0xC8);

	Outfit_t currentOutfit = player->getDefaultOutfit();
	AddOutfit(msg, currentOutfit);

	uint16_t firstOutfit;
	uint16_t lastOutfit;
	switch (player->getSex()) {
		case PLAYERSEX_FEMALE: {
			firstOutfit = 136;
			lastOutfit = player->isPremium() ? 142 : 139;
			break;
		}

		case PLAYERSEX_MALE: {
			firstOutfit = 128;
			lastOutfit = player->isPremium() ? 134 : 131;
			break;
		}

		default: {
			firstOutfit = 128;
			lastOutfit = 134;
		}
	}

	if (version <= 760) {
		msg.addByte(firstOutfit);
		msg.addByte(lastOutfit);
	} else {
		msg.add<uint16_t>(firstOutfit);
		msg.add<uint16_t>(lastOutfit);
	}

	writeToOutputBuffer(msg);
}

void ProtocolGame::sendPodiumWindow(const Item*)
{
	// Podiums are unknown to the 7.6 client.
}

void ProtocolGame::sendUpdatedVIPStatus(uint32_t guid, VipStatus_t newStatus)
{
	// 7.x: 0xD3 = buddy went online, 0xD4 = buddy went offline; guid only.
	NetworkMessage msg;
	msg.addByte(newStatus == VIPSTATUS_OFFLINE ? 0xD4 : 0xD3);
	msg.add<uint32_t>(guid);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, const std::string&, uint32_t, bool,
                           VipStatus_t status)
{
	// 7.x VIP entry: guid, name, online flag.
	NetworkMessage msg;
	msg.addByte(0xD2);
	msg.add<uint32_t>(guid);
	msg.addString(name);
	msg.addByte(status == VIPSTATUS_OFFLINE ? 0x00 : 0x01);
	writeToOutputBuffer(msg);
}

void ProtocolGame::sendVIPEntries()
{
	const std::forward_list<VIPEntry>& vipEntries = IOLoginData::getVIPEntries(player->getAccount());

	for (const VIPEntry& entry : vipEntries) {
		VipStatus_t vipStatus = VIPSTATUS_ONLINE;

		Player* vipPlayer = g_game.getPlayerByGUID(entry.guid);

		if (!vipPlayer || !player->canSeeCreature(vipPlayer)) {
			vipStatus = VIPSTATUS_OFFLINE;
		}

		sendVIP(entry.guid, entry.name, entry.description, entry.icon, entry.notify, vipStatus);
	}
}

void ProtocolGame::sendItemClasses()
{
	// 0x86 item classes/tiers are unknown to the 7.6 client.
}

void ProtocolGame::sendSpellCooldown(uint8_t, uint32_t)
{
	// 0xA4 spell cooldowns are unknown to the 7.6 client.
}

void ProtocolGame::sendSpellGroupCooldown(SpellGroup_t, uint32_t)
{
	// 0xA5 spell group cooldowns are unknown to the 7.6 client.
}

void ProtocolGame::sendUseItemCooldown(uint32_t)
{
	// 0xA6 item cooldowns are unknown to the 7.6 client.
}

void ProtocolGame::sendSupplyUsed(const uint16_t)
{
	// 0xCE supply-used is unknown to the 7.6 client.
}

void ProtocolGame::sendModalWindow(const ModalWindow&)
{
	// Modal windows are unknown to the 7.6 client.
}

////////////// Add common messages
void ProtocolGame::AddCreature(NetworkMessage& msg, const Creature* creature, bool known, uint32_t remove)
{
	const Player* otherPlayer = creature->getPlayer();

	if (known) {
		msg.add<uint16_t>(0x62);
		msg.add<uint32_t>(creature->getID());
	} else {
		msg.add<uint16_t>(0x61);
		msg.add<uint32_t>(remove);
		msg.add<uint32_t>(creature->getID());
		msg.addString(creature->getName());
	}

	if (creature->isHealthHidden()) {
		msg.addByte(0x00);
	} else {
		msg.addByte(std::ceil(
		    (static_cast<double>(creature->getHealth()) / std::max<int32_t>(creature->getMaxHealth(), 1)) * 100));
	}

	msg.addByte(creature->getDirection());

	if (!creature->isInGhostMode() && !creature->isInvisible()) {
		const Outfit_t& outfit = creature->getCurrentOutfit();
		AddOutfit(msg, outfit);
	} else {
		static Outfit_t outfit;
		AddOutfit(msg, outfit);
	}

	LightInfo lightInfo = creature->getCreatureLight();
	msg.addByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
	msg.addByte(lightInfo.color);

	msg.add<uint16_t>(creature->getStepSpeed());

	msg.addByte(player->getSkullClient(creature));
	msg.addByte(player->getPartyShield(otherPlayer));
}

void ProtocolGame::AddPlayerStats(NetworkMessage& msg)
{
	// 7.x stats: u16 hp/maxhp, u16 cap, u32 exp (capped), u16 level + %,
	// u16 mana/maxmana, u8 maglevel + %, u8 soul.
	msg.addByte(0xA0);

	msg.add<uint16_t>(std::min<int32_t>(player->getHealth(), std::numeric_limits<uint16_t>::max()));
	msg.add<uint16_t>(std::min<int32_t>(player->getMaxHealth(), std::numeric_limits<uint16_t>::max()));

	msg.add<uint16_t>(player->hasFlag(PlayerFlag_HasInfiniteCapacity) ? 10000 : (player->getFreeCapacity() / 100));

	// the 7.6 client debugs if the value is higher than 0x7FFFFFFF
	msg.add<uint32_t>(std::min<uint64_t>(player->getExperience(), 0x7FFFFFFF));

	msg.add<uint16_t>(player->getLevel());
	msg.addByte(player->getLevelPercent());

	msg.add<uint16_t>(std::min<int32_t>(player->getMana(), std::numeric_limits<uint16_t>::max()));
	msg.add<uint16_t>(std::min<int32_t>(player->getMaxMana(), std::numeric_limits<uint16_t>::max()));

	msg.addByte(std::min<uint32_t>(player->getMagicLevel(), std::numeric_limits<uint8_t>::max()));
	msg.addByte(player->getMagicLevelPercent());

	msg.addByte(player->getSoul());
}

void ProtocolGame::AddPlayerSkills(NetworkMessage& msg)
{
	// 7.x skills: level byte + percent byte per skill, nothing else.
	msg.addByte(0xA1);

	for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; ++i) {
		msg.addByte(std::min<int32_t>(player->getSkillLevel(i), std::numeric_limits<uint8_t>::max()));
		msg.addByte(player->getSkillPercent(i));
	}
}

void ProtocolGame::AddOutfit(NetworkMessage& msg, const Outfit_t& outfit)
{
	// The 7.6 client reads lookType as a single byte; CipSoft widened it to
	// u16 with 7.7 — sending the u16 to a 760 session desyncs the stream one
	// byte per creature ("debug assertion in module Container"). No addons,
	// no mount either way.
	if (version <= 760) {
		msg.addByte(std::min<uint16_t>(outfit.lookType, std::numeric_limits<uint8_t>::max()));
	} else {
		msg.add<uint16_t>(outfit.lookType);
	}
	if (outfit.lookType != 0) {
		msg.addByte(outfit.lookHead);
		msg.addByte(outfit.lookBody);
		msg.addByte(outfit.lookLegs);
		msg.addByte(outfit.lookFeet);
	} else {
		msg.addItemId(outfit.lookTypeEx);
	}
}

void ProtocolGame::AddWorldLight(NetworkMessage& msg, LightInfo lightInfo)
{
	msg.addByte(0x82);
	msg.addByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.addByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(NetworkMessage& msg, const Creature* creature)
{
	LightInfo lightInfo = creature->getCreatureLight();

	msg.addByte(0x8D);
	msg.add<uint32_t>(creature->getID());
	msg.addByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg.addByte(lightInfo.color);
}

// tile
void ProtocolGame::RemoveTileThing(NetworkMessage& msg, const Position& pos, uint32_t stackpos)
{
	if (stackpos >= MAX_STACKPOS) {
		return;
	}

	msg.addByte(0x6C);
	msg.addPosition(pos);
	msg.addByte(stackpos);
}

void ProtocolGame::RemoveTileCreature(NetworkMessage& msg, const Creature* creature, const Position& pos,
                                      uint32_t stackpos)
{
	if (stackpos < MAX_STACKPOS) {
		RemoveTileThing(msg, pos, stackpos);
		return;
	}

	msg.addByte(0x6C);
	msg.add<uint16_t>(0xFFFF);
	msg.add<uint32_t>(creature->getID());
}

void ProtocolGame::MoveUpCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos,
                                  const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	// floor change up
	msg.addByte(0xBE);

	// going to surface
	if (newPos.z == 7) {
		int32_t skip = -1;

		// floor 7 and 6 already set
		for (int i = 5; i >= 0; --i) {
			GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, i,
			                    (Map::maxClientViewportX * 2) + 2, (Map::maxClientViewportY * 2) + 2, 8 - i, skip);
		}
		if (skip >= 0) {
			msg.addByte(skip);
			msg.addByte(0xFF);
		}
	}
	// underground, going one floor up (still underground)
	else if (newPos.z > 7) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY,
		                    oldPos.getZ() - 3, (Map::maxClientViewportX * 2) + 2, (Map::maxClientViewportY * 2) + 2, 3,
		                    skip);

		if (skip >= 0) {
			msg.addByte(skip);
			msg.addByte(0xFF);
		}
	}

	// moving up a floor up makes us out of sync
	// west
	msg.addByte(0x68);
	GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y - (Map::maxClientViewportY - 1), newPos.z, 1,
	                  (Map::maxClientViewportY * 2) + 2, msg);

	// north
	msg.addByte(0x65);
	GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z,
	                  (Map::maxClientViewportX * 2) + 2, 1, msg);
}

void ProtocolGame::MoveDownCreature(NetworkMessage& msg, const Creature* creature, const Position& newPos,
                                    const Position& oldPos)
{
	if (creature != player) {
		return;
	}

	// floor change down
	msg.addByte(0xBF);

	// going from surface to underground
	if (newPos.z == 8) {
		int32_t skip = -1;

		for (int i = 0; i < 3; ++i) {
			GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY,
			                    newPos.z + i, (Map::maxClientViewportX * 2) + 2, (Map::maxClientViewportY * 2) + 2,
			                    -i - 1, skip);
		}
		if (skip >= 0) {
			msg.addByte(skip);
			msg.addByte(0xFF);
		}
	}
	// going further down
	else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) {
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z + 2,
		                    (Map::maxClientViewportX * 2) + 2, (Map::maxClientViewportY * 2) + 2, -3, skip);

		if (skip >= 0) {
			msg.addByte(skip);
			msg.addByte(0xFF);
		}
	}

	// moving down a floor makes us out of sync
	// east
	msg.addByte(0x66);
	GetMapDescription(oldPos.x + (Map::maxClientViewportX + 1), oldPos.y - (Map::maxClientViewportY + 1), newPos.z, 1,
	                  (Map::maxClientViewportY * 2) + 2, msg);

	// south
	msg.addByte(0x67);
	GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y + (Map::maxClientViewportY + 1), newPos.z,
	                  (Map::maxClientViewportX * 2) + 2, 1, msg);
}

void ProtocolGame::AddShopItem(NetworkMessage& msg, const ShopInfo& item)
{
	const ItemType& it = Item::items[item.itemId];
	msg.add<uint16_t>(it.clientId);

	if (it.isSplash() || it.isFluidContainer()) {
		msg.addByte(serverFluidToClient(item.subType));
	} else {
		msg.addByte(0x00);
	}

	msg.addString(item.realName);
	msg.add<uint32_t>(it.weight);
	msg.add<uint32_t>(std::max<uint32_t>(item.buyPrice, 0));
	msg.add<uint32_t>(std::max<uint32_t>(item.sellPrice, 0));
}

void ProtocolGame::parseExtendedOpcode(NetworkMessage& msg)
{
	uint8_t opcode = msg.getByte();
	auto buffer = msg.getString();

	// process additional opcodes via lua script event
	g_dispatcher.addTask([=, playerID = player->getID(), buffer = std::string{buffer}]() {
		g_game.parsePlayerExtendedOpcode(playerID, opcode, buffer);
	});
}
