// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocol.h"

#include "outputmessage.h"
#include "rsa.h"
#include "xtea.h"

namespace {

void XTEA_encrypt(OutputMessage& msg, const xtea::round_keys& key)
{
	// The message must be a multiple of 8
	size_t paddingBytes = msg.getLength() % 8u;
	if (paddingBytes != 0) {
		msg.addPaddingBytes(8 - paddingBytes);
	}

	uint8_t* buffer = msg.getOutputBuffer();
	xtea::encrypt(buffer, msg.getLength(), key);
}

bool XTEA_decrypt(NetworkMessage& msg, const xtea::round_keys& key)
{
	// Plaintext u16-length framing (no checksum): the encrypted payload
	// starts right past the 2-byte header and carries its own inner length.
	if (((msg.getLength() - 2) & 7) != 0) {
		return false;
	}

	uint8_t* buffer = msg.getRemainingBuffer();
	xtea::decrypt(buffer, msg.getLength() - 2, key);

	uint16_t innerLength = msg.get<uint16_t>();
	if (innerLength > msg.getLength() - 4) {
		return false;
	}

	msg.setLength(innerLength);
	return true;
}

} // namespace

void Protocol::onSendMessage(const OutputMessage_ptr& msg)
{
	// Protocol 7.60 predates RSA/XTEA/checksums: those sessions are
	// plaintext, framed only by the u16 length header. 7.61+ sessions XTEA
	// encrypt the length-framed message and prepend an outer length header
	// (still no checksum at 7.72).
	if (!rawMessages) {
		msg->writeMessageLength();

		if (encryptionEnabled) {
			XTEA_encrypt(*msg, key);
			msg->writeMessageLength();
		}
	}
}

void Protocol::onRecvMessage(NetworkMessage& msg)
{
	if (encryptionEnabled && !XTEA_decrypt(msg, key)) {
		return;
	}

	parsePacket(msg);
}

bool Protocol::RSA_decrypt(NetworkMessage& msg)
{
	if ((msg.getLength() - msg.getBufferPosition()) < 128) {
		return false;
	}

	tfs::rsa::decrypt(msg.getRemainingBuffer(), 128);
	return msg.getByte() == 0;
}

OutputMessage_ptr Protocol::getOutputBuffer(int32_t size)
{
	// dispatcher thread
	if (!outputBuffer) {
		outputBuffer = OutputMessagePool::getOutputMessage();
	} else if ((outputBuffer->getLength() + size) > NetworkMessage::MAX_PROTOCOL_BODY_LENGTH) {
		send(outputBuffer);
		outputBuffer = OutputMessagePool::getOutputMessage();
	}
	return outputBuffer;
}

Connection::Address Protocol::getIP() const
{
	if (auto connection = getConnection()) {
		return connection->getIP();
	}

	return {};
}
