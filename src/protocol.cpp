// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocol.h"

#include "outputmessage.h"

void Protocol::onSendMessage(const OutputMessage_ptr& msg)
{
	// Protocol 7.60 predates RSA/XTEA/checksums: the whole session is
	// plaintext, framed only by the u16 length header.
	if (!rawMessages) {
		msg->writeMessageLength();
	}
}

void Protocol::onRecvMessage(NetworkMessage& msg) { parsePacket(msg); }

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
