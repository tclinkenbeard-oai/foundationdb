/*
 * UDPWorkload.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2026 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/FDBTypes.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/ActorCollection.h"
#include "flow/Arena.h"
#include "flow/CoroUtils.h"
#include "flow/Error.h"
#include "flow/flow.h"
#include "flow/IConnection.h"
#include "flow/IRandom.h"
#include "flow/IUDPSocket.h"
#include "flow/network.h"
#include "flow/serialize.h"

#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

struct UDPWorkload : TestWorkload {
	static constexpr auto NAME = "UDPWorkload";

	Key keyPrefix;
	double runFor;
	int minPort, maxPort;

	NetworkAddress serverAddress;
	Reference<IUDPSocket> serverSocket;
	std::vector<NetworkAddress> remotes;
	std::unordered_map<NetworkAddress, unsigned> sent, received, acked, successes;
	PromiseStream<NetworkAddress> toAck;

	UDPWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		keyPrefix = getOption(options, "keyPrefix"_sr, "/udp/"_sr);
		runFor = getOption(options, "runFor"_sr, 60.0);
		minPort = getOption(options, "minPort"_sr, 5000);
		maxPort = getOption(options, "minPort"_sr, 6000);
		for (auto p : { minPort, maxPort }) {
			ASSERT(p > 0 && p < std::numeric_limits<unsigned short>::max());
		}
	}

	static Future<Void> setupImpl(UDPWorkload* self, Database cx) {
		NetworkAddress localAddress(g_network->getLocalAddress().ip,
		                            deterministicRandom()->randomInt(self->minPort, self->maxPort + 1),
		                            true,
		                            false);
		Key key = self->keyPrefix.withSuffix(BinaryWriter::toValue(self->clientId, Unversioned()));
		Value serializedLocalAddress = BinaryWriter::toValue(localAddress, IncludeVersion());
		ReadYourWritesTransaction tr(cx);
		auto createSocket = INetworkConnections::net()->createUDPSocket(localAddress.isV6());
		self->serverSocket = co_await createSocket;
		self->serverSocket->bind(localAddress);
		self->serverAddress = localAddress;
		while (true) {
			Error err;
			try {
				auto getKey = tr.get(key);
				Optional<Value> v = co_await getKey;
				if (v.present()) {
					co_return;
				}
				tr.set(key, serializedLocalAddress);
				auto commit = tr.commit();
				co_await commit;
				co_return;
			} catch (Error& e) {
				err = e;
			}
			auto onError = tr.onError(err);
			co_await onError;
		}
	}

	Future<Void> setup(Database const& cx) override { return setupImpl(this, cx); }

	class Message {
		int _type = 0;

	public:
		enum class Type : uint8_t { PING, PONG };

		Message() = default;

		explicit Message(Type t) {
			switch (t) {
			case Type::PING:
				_type = 0;
				break;
			case Type::PONG:
				_type = 1;
				break;
			default:
				UNSTOPPABLE_ASSERT(false);
			}
		}

		Type type() const {
			switch (_type) {
			case 0:
				return Type::PING;
			case 1:
				return Type::PONG;
			default:
				UNSTOPPABLE_ASSERT(false);
			}
		}

		template <class Ar>
		void serialize(Ar& ar) {
			serializer(ar, _type);
		}
	};

	static Message ping() { return Message{ Message::Type::PING }; }
	static Message pong() { return Message{ Message::Type::PONG }; }

	static Future<Void> receiver(UDPWorkload* self) {
		Standalone<StringRef> packetString = makeString(IUDPSocket::MAX_PACKET_SIZE);
		uint8_t* packet = mutateString(packetString);
		NetworkAddress peerAddress;
		while (true) {
			auto receive = self->serverSocket->receiveFrom(packet, packet + IUDPSocket::MAX_PACKET_SIZE, &peerAddress);
			int sz = co_await receive;
			auto msg = BinaryReader::fromStringRef<Message>(packetString.substr(0, sz), IncludeVersion());
			if (msg.type() == Message::Type::PONG) {
				self->successes[peerAddress] += 1;
			} else if (msg.type() == Message::Type::PING) {
				self->received[peerAddress] += 1;
				self->toAck.send(peerAddress);
			} else {
				UNSTOPPABLE_ASSERT(false);
			}
		}
	}

	static Future<Void> serverSender(UDPWorkload* self) {
		Standalone<StringRef> packetString;
		NetworkAddress peer;
		while (true) {
			auto sendDelay = delay(0.1);
			auto ack = self->toAck.getFuture();
			auto next = race(sendDelay, ack);
			auto choice = co_await next;
			if (choice.index() == 0) {
				peer = deterministicRandom()->randomChoice(self->remotes);
				packetString = BinaryWriter::toValue(ping(), IncludeVersion());
				self->sent[peer] += 1;
			} else if (choice.index() == 1) {
				peer = std::get<1>(std::move(choice));
				packetString = BinaryWriter::toValue(pong(), IncludeVersion());
				self->acked[peer] += 1;
			} else {
				UNREACHABLE();
			}

			auto send = self->serverSocket->sendTo(packetString.begin(), packetString.end(), peer);
			int res = co_await send;
			ASSERT(res == packetString.size());
		}
	}

	static Future<Void> clientReceiver(UDPWorkload* self, Reference<IUDPSocket> socket, Future<Void> done) {
		Standalone<StringRef> packetString = makeString(IUDPSocket::MAX_PACKET_SIZE);
		uint8_t* packet = mutateString(packetString);
		NetworkAddress peer;
		Future<Void> finished = Never();
		while (true) {
			auto receive = socket->receiveFrom(packet, packet + IUDPSocket::MAX_PACKET_SIZE, &peer);
			auto next = race(receive, done, finished);
			auto choice = co_await next;
			if (choice.index() == 0) {
				int sz = std::get<0>(std::move(choice));
				auto res = BinaryReader::fromStringRef<Message>(packetString.substr(0, sz), IncludeVersion());
				ASSERT(res.type() == Message::Type::PONG);
				self->successes[peer] += 1;
			} else if (choice.index() == 1) {
				finished = delay(1.0);
				done = Never();
			} else if (choice.index() == 2) {
				co_return;
			} else {
				UNREACHABLE();
			}
		}
	}

	static Future<Void> clientSender(UDPWorkload* self) {
		AsyncVar<Reference<IUDPSocket>> socket;
		Standalone<StringRef> sendString;
		ActorCollection actors(false);
		NetworkAddress peer;

		while (true) {
			auto tick = delay(0.1);
			auto actorResult = actors.getResult();
			auto next = race(tick, actorResult);
			auto choice = co_await next;
			if (choice.index() == 1) {
				UNSTOPPABLE_ASSERT(false);
			}

			if (!socket.get().isValid() || deterministicRandom()->random01() < 0.05) {
				peer = deterministicRandom()->randomChoice(self->remotes);
				auto createSocket = INetworkConnections::net()->createUDPSocket(peer);
				Reference<IUDPSocket> s = co_await createSocket;
				socket.set(s);
				socket = s;
				auto changed = socket.onChange();
				actors.add(clientReceiver(self, socket.get(), changed));
			}

			sendString = BinaryWriter::toValue(ping(), IncludeVersion());
			auto send = socket.get()->send(sendString.begin(), sendString.end());
			int res = co_await send;
			ASSERT(res == sendString.size());
			self->sent[peer] += 1;
		}
	}

	static Future<Void> startImpl(UDPWorkload* self, Database cx) {
		ReadYourWritesTransaction tr(cx);
		while (true) {
			Error err;
			try {
				auto getRange = tr.getRange(prefixRange(self->keyPrefix), CLIENT_KNOBS->TOO_MANY);
				RangeResult range = co_await getRange;
				ASSERT(!range.more);
				self->remotes.clear();
				for (auto const& p : range) {
					auto cID = BinaryReader::fromStringRef<decltype(self->clientId)>(
					    p.key.removePrefix(self->keyPrefix), Unversioned());
					if (cID != self->clientId) {
						self->remotes.emplace_back(
						    BinaryReader::fromStringRef<NetworkAddress>(p.value, IncludeVersion()));
					}
				}
				break;
			} catch (Error& e) {
				err = e;
			}
			auto onError = tr.onError(err);
			co_await onError;
		}

		ActorCollection actors(false);
		actors.add(clientSender(self));
		actors.add(serverSender(self));
		actors.add(receiver(self));
		auto actorResult = actors.getResult();
		co_await actorResult;
		UNSTOPPABLE_ASSERT(false);
	}

	Future<Void> start(Database const& cx) override { return delay(runFor) || startImpl(this, cx); }
	Future<bool> check(Database const& cx) override { return true; }

	void getMetrics(std::vector<PerfMetric>& m) override {
		unsigned totalReceived = 0, totalSent = 0, totalAcked = 0, totalSuccess = 0;
		for (const auto& [_destination, sentCount] : sent) {
			totalSent += sentCount;
		}
		for (const auto& [_source, receivedCount] : received) {
			totalReceived += receivedCount;
		}
		for (const auto& [_destination, ackedCount] : acked) {
			totalAcked += ackedCount;
		}
		for (const auto& [_destination, successCount] : successes) {
			totalSuccess += successCount;
		}
		m.emplace_back("Sent", totalSent, Averaged::False);
		m.emplace_back("Received", totalReceived, Averaged::False);
		m.emplace_back("Acknknowledged", totalAcked, Averaged::False);
		m.emplace_back("Successes", totalSuccess, Averaged::False);
	}
};

} // namespace

WorkloadFactory<UDPWorkload> UDPWorkloadFactory;
