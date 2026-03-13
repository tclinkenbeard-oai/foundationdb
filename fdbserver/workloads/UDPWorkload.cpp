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

	Future<Void> setupImpl(Database cx) {
		NetworkAddress localAddress(
		    g_network->getLocalAddress().ip, deterministicRandom()->randomInt(minPort, maxPort + 1), true, false);
		Key key = keyPrefix.withSuffix(BinaryWriter::toValue(clientId, Unversioned()));
		Value serializedLocalAddress = BinaryWriter::toValue(localAddress, IncludeVersion());
		ReadYourWritesTransaction tr(cx);
		serverSocket = co_await INetworkConnections::net()->createUDPSocket(localAddress.isV6());
		serverSocket->bind(localAddress);
		serverAddress = localAddress;
		while (true) {
			Error err;
			try {
				Optional<Value> v = co_await tr.get(key);
				if (v.present()) {
					co_return;
				}
				tr.set(key, serializedLocalAddress);
				co_await tr.commit();
				co_return;
			} catch (Error& e) {
				err = e;
			}
			co_await tr.onError(err);
		}
	}

	Future<Void> setup(Database const& cx) override { return setupImpl(cx); }

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

	Future<Void> receiver() {
		Standalone<StringRef> packetString = makeString(IUDPSocket::MAX_PACKET_SIZE);
		uint8_t* packet = mutateString(packetString);
		NetworkAddress peerAddress;
		while (true) {
			int sz = co_await serverSocket->receiveFrom(packet, packet + IUDPSocket::MAX_PACKET_SIZE, &peerAddress);
			auto msg = BinaryReader::fromStringRef<Message>(packetString.substr(0, sz), IncludeVersion());
			if (msg.type() == Message::Type::PONG) {
				successes[peerAddress] += 1;
			} else if (msg.type() == Message::Type::PING) {
				received[peerAddress] += 1;
				toAck.send(peerAddress);
			} else {
				UNSTOPPABLE_ASSERT(false);
			}
		}
	}

	Future<Void> serverSender(std::vector<NetworkAddress>& remotes) {
		Standalone<StringRef> packetString;
		NetworkAddress peer;
		while (true) {
			auto choice = co_await race(delay(0.1), toAck.getFuture());
			if (choice.index() == 0) {
				peer = deterministicRandom()->randomChoice(remotes);
				packetString = BinaryWriter::toValue(ping(), IncludeVersion());
				sent[peer] += 1;
			} else if (choice.index() == 1) {
				peer = std::get<1>(std::move(choice));
				packetString = BinaryWriter::toValue(pong(), IncludeVersion());
				acked[peer] += 1;
			} else {
				UNREACHABLE();
			}

			int res = co_await serverSocket->sendTo(packetString.begin(), packetString.end(), peer);
			ASSERT(res == packetString.size());
		}
	}

	Future<Void> clientReceiver(Reference<IUDPSocket> socket, Future<Void> done) {
		Standalone<StringRef> packetString = makeString(IUDPSocket::MAX_PACKET_SIZE);
		uint8_t* packet = mutateString(packetString);
		NetworkAddress peer;
		Future<Void> finished = Never();
		while (true) {
			auto choice =
			    co_await race(socket->receiveFrom(packet, packet + IUDPSocket::MAX_PACKET_SIZE, &peer), done, finished);
			if (choice.index() == 0) {
				int sz = std::get<0>(std::move(choice));
				auto res = BinaryReader::fromStringRef<Message>(packetString.substr(0, sz), IncludeVersion());
				ASSERT(res.type() == Message::Type::PONG);
				successes[peer] += 1;
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

	Future<Void> clientSender(std::vector<NetworkAddress>& remotes) {
		AsyncVar<Reference<IUDPSocket>> socket;
		Standalone<StringRef> sendString;
		ActorCollection actors(false);
		NetworkAddress peer;

		while (true) {
			auto choice = co_await race(delay(0.1), actors.getResult());
			if (choice.index() == 0) {
			} else if (choice.index() == 1) {
				UNSTOPPABLE_ASSERT(false);
			} else {
				UNREACHABLE();
			}

			if (!socket.get().isValid() || deterministicRandom()->random01() < 0.05) {
				peer = deterministicRandom()->randomChoice(remotes);
				Reference<IUDPSocket> s = co_await INetworkConnections::net()->createUDPSocket(peer);
				socket.set(s);
				socket = s;
				actors.add(clientReceiver(socket.get(), socket.onChange()));
			}

			sendString = BinaryWriter::toValue(ping(), IncludeVersion());
			int res = co_await socket.get()->send(sendString.begin(), sendString.end());
			ASSERT(res == sendString.size());
			sent[peer] += 1;
		}
	}

	Future<Void> startImpl(Database cx) {
		ReadYourWritesTransaction tr(cx);
		std::vector<NetworkAddress> remotes;
		while (true) {
			Error err;
			try {
				RangeResult range = co_await tr.getRange(prefixRange(keyPrefix), CLIENT_KNOBS->TOO_MANY);
				ASSERT(!range.more);
				remotes.clear();
				for (auto const& p : range) {
					auto cID =
					    BinaryReader::fromStringRef<decltype(clientId)>(p.key.removePrefix(keyPrefix), Unversioned());
					if (cID != clientId) {
						remotes.emplace_back(BinaryReader::fromStringRef<NetworkAddress>(p.value, IncludeVersion()));
					}
				}
				break;
			} catch (Error& e) {
				err = e;
			}
			co_await tr.onError(err);
		}

		co_await (clientSender(remotes) && serverSender(remotes) && receiver());
		UNSTOPPABLE_ASSERT(false);
	}

	Future<Void> start(Database const& cx) override { return delay(runFor) || startImpl(cx); }
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
