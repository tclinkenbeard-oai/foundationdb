/*
 * LogSystemRecoveryTests.cpp
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

#include "fdbserver/logsystem/LogSystem.h"
#include "fdbserver/logsystem/LogSystemConsumer.h"
#include "fdbclient/SystemData.h"
#include "flow/UnitTest.h"

namespace {

Reference<LogSet> makeSingleLogSet(const std::vector<TLogInterface>& tlogs, bool isLocal = true) {
	auto logSet = makeReference<LogSet>();
	logSet->isLocal = isLocal;
	for (const auto& tlog : tlogs) {
		logSet->logServers.push_back(
		    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(tlog)));
	}
	return logSet;
}

std::tuple<int, std::vector<TLogLockResult>, bool> makeLogGroupResults(
    int replicationFactor,
    const std::vector<std::vector<UnknownCommittedVersions>>& perTLogUCV,
    const std::vector<TLogInterface>& tlogs,
    bool nonAvailableTLogsCompletePolicy = true,
    const std::vector<Version>& knownCommitted = {}) {
	std::vector<TLogLockResult> lockResults;
	lockResults.reserve(tlogs.size());
	for (int i = 0; i < tlogs.size(); ++i) {
		TLogLockResult result;
		result.logId = tlogs[i].id();
		result.knownCommittedVersion = (i < knownCommitted.size()) ? knownCommitted[i] : 0;
		for (const auto& ucv : perTLogUCV[i]) {
			result.unknownCommittedVersions.push_back(ucv);
		}
		lockResults.push_back(result);
	}
	return std::make_tuple(replicationFactor, std::move(lockResults), nonAvailableTLogsCompletePolicy);
}

} // namespace

void forceLinkLogSystemRecoveryTests() {}

TEST_CASE("/LogSystem/GetLogsValue/RoleLocalities") {
	const UID liveLogId(1, 1);
	const UID unavailableLogId(2, 1);
	const UID incompleteLogId(2, 2);
	const NetworkAddress liveAddress(IPAddress(0x0a000001), 4500);
	LocalityData storedLiveLocality;
	storedLiveLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("stale-process"_sr));
	LocalityData liveLocality;
	liveLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("rejoined-process"_sr));
	LocalityData unavailableLocality;
	unavailableLocality.set(LocalityData::keyMachineId, Standalone<StringRef>("unavailable-machine"_sr));
	LocalityData previousUnavailableLocality = unavailableLocality;
	previousUnavailableLocality.set("rack"_sr, Standalone<StringRef>("unavailable-rack"_sr));

	TLogInterface liveLog(liveLogId, UID(1, 2), liveLocality);
	liveLog.peekMessages = RequestStream<struct TLogPeekRequest>(Endpoint({ liveAddress }, UID(1, 3)));
	auto currentSet = makeReference<LogSet>();
	currentSet->isLocal = true;
	currentSet->logServers.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(liveLog)));
	currentSet->tLogLocalities.push_back(storedLiveLocality);

	auto oldSet = makeReference<LogSet>();
	oldSet->logServers.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(unavailableLogId)));
	oldSet->logServers.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(incompleteLogId)));
	oldSet->tLogLocalities.push_back(unavailableLocality);
	oldSet->tLogLocalities.push_back(LocalityData());
	OldLogData oldData;
	oldData.tLogs.push_back(oldSet);

	LogSystem logSystem(UID(), LocalityData(), LogEpoch(1));
	logSystem.tLogs.push_back(currentSet);
	logSystem.oldLogData.push_back(oldData);
	LogsValue previousLogs;
	previousLogs.logLocalities[unavailableLogId] = previousUnavailableLocality;
	LogsValue logs = decodeLogsValue(logSystem.getLogsValue(previousLogs));

	ASSERT(logs.logs.size() == 1);
	ASSERT(logs.logs[0].first == liveLogId);
	ASSERT(logs.logs[0].second == liveAddress);
	ASSERT(logs.oldLogs.size() == 2);
	ASSERT(logs.oldLogs[0].first == unavailableLogId);
	ASSERT(logs.oldLogs[0].second == NetworkAddress());
	ASSERT(logs.oldLogs[1].first == incompleteLogId);
	ASSERT(logs.oldLogs[1].second == NetworkAddress());
	ASSERT(logs.logLocalities.at(liveLogId).processId().get() == "rejoined-process"_sr);
	ASSERT(logs.logLocalities.at(unavailableLogId).machineId().get() == "unavailable-machine"_sr);
	ASSERT(logs.logLocalities.at(unavailableLogId).get("rack"_sr).get() == "unavailable-rack"_sr);
	ASSERT(!logs.incompleteLogLocalities.contains(liveLogId));
	ASSERT(!logs.incompleteLogLocalities.contains(unavailableLogId));
	ASSERT(logs.incompleteLogLocalities.contains(incompleteLogId));

	return Void();
}

TEST_CASE("/LogSystem/TrackRejoins/RefreshLocality") {
	const UID logId(1, 1);
	const NetworkAddress logAddress(IPAddress(0x0a000001), 4500);
	const Endpoint peekEndpoint({ logAddress }, UID(1, 2));
	const Endpoint commitEndpoint({ logAddress }, UID(1, 3));
	LocalityData staleLocality;
	staleLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("stale-process"_sr));
	TLogInterface staleLog(logId, UID(1, 4), staleLocality);
	staleLog.peekMessages = RequestStream<struct TLogPeekRequest>(peekEndpoint);
	staleLog.commit = RequestStream<struct TLogCommitRequest>(commitEndpoint);
	auto logVar = makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(staleLog));

	PromiseStream<TLogRejoinRequest> rejoins;
	Future<Void> trackRejoins = LogSystem::trackRejoins(UID(), { logVar }, rejoins.getFuture());
	LocalityData rejoinedLocality;
	rejoinedLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("rejoined-process"_sr));
	rejoinedLocality.set("rack"_sr, Standalone<StringRef>("rejoined-rack"_sr));
	TLogInterface rejoinedLog(logId, UID(1, 4), rejoinedLocality);
	rejoinedLog.peekMessages = RequestStream<struct TLogPeekRequest>(peekEndpoint);
	rejoinedLog.commit = RequestStream<struct TLogCommitRequest>(commitEndpoint);
	Future<Void> changed = logVar->onChange();
	rejoins.send(TLogRejoinRequest(rejoinedLog));
	co_await changed;

	ASSERT(logVar->get().interf().filteredLocality.processId().get() == "rejoined-process"_sr);
	ASSERT(logVar->get().interf().filteredLocality.get("rack"_sr).get() == "rejoined-rack"_sr);
	trackRejoins.cancel();

	co_return;
}

TEST_CASE("/LogSystem/CoreState/OldLogLocalities") {
	const UID oldLogId(2, 1);
	const NetworkAddress oldAddress(IPAddress(0x0a000002), 4500);
	LocalityData staleLocality;
	staleLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("stale-process"_sr));
	LocalityData rejoinedLocality;
	rejoinedLocality.set(LocalityData::keyProcessId, Standalone<StringRef>("rejoined-process"_sr));
	rejoinedLocality.set("rack"_sr, Standalone<StringRef>("rejoined-rack"_sr));
	TLogInterface oldLog(oldLogId, UID(2, 2), rejoinedLocality);
	oldLog.peekMessages = RequestStream<struct TLogPeekRequest>(Endpoint({ oldAddress }, UID(2, 3)));

	auto oldSet = makeReference<LogSet>();
	oldSet->logServers.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(oldLog)));
	oldSet->tLogLocalities.push_back(staleLocality);
	oldSet->tLogExclusionLocalities.push_back(staleLocality);
	OldLogData oldData;
	oldData.tLogs.push_back(oldSet);
	LogSystem source(UID(), LocalityData(), LogEpoch(1));
	source.oldLogData.push_back(oldData);
	LogSystemConfig config = source.getLogSystemConfig();
	ASSERT(config.oldTLogs.size() == 1);
	ASSERT(config.oldTLogs[0].tLogs.size() == 1);
	ASSERT(config.oldTLogs[0].tLogs[0].tLogLocalities[0].get("rack"_sr).get() == "rejoined-rack"_sr);
	TLogSet changedLogSet = config.oldTLogs[0].tLogs[0];
	changedLogSet.tLogLocalities[0].set("rack"_sr, Standalone<StringRef>("changed-rack"_sr));
	ASSERT(!(changedLogSet == config.oldTLogs[0].tLogs[0]));

	DBCoreState coreState;
	source.toCoreState(coreState);
	ASSERT(coreState.oldTLogData.size() == 1);
	ASSERT(coreState.oldTLogData[0].tLogs.size() == 1);
	ASSERT(coreState.oldTLogData[0].tLogs[0].tLogLocalities[0].get("rack"_sr).get() == "rejoined-rack"_sr);
	CoreTLogSet changedCoreSet = coreState.oldTLogData[0].tLogs[0];
	changedCoreSet.tLogLocalities[0].set("rack"_sr, Standalone<StringRef>("changed-rack"_sr));
	ASSERT(!(changedCoreSet == coreState.oldTLogData[0].tLogs[0]));

	LogSystem recovered(UID(), LocalityData(), LogEpoch(2));
	recovered.oldLogData.emplace_back(coreState.oldTLogData[0]);
	LogsValue logs = decodeLogsValue(recovered.getLogsValue());
	ASSERT(logs.oldLogs.size() == 1);
	ASSERT(logs.oldLogs[0].first == oldLogId);
	ASSERT(logs.oldLogs[0].second == NetworkAddress());
	ASSERT(logs.logLocalities.at(oldLogId).processId().get() == "rejoined-process"_sr);
	ASSERT(logs.logLocalities.at(oldLogId).get("rack"_sr).get() == "rejoined-rack"_sr);
	ASSERT(logs.incompleteLogLocalities.contains(oldLogId));

	return Void();
}

TEST_CASE("/LogSystem/PopLogRouter/CurrentGenerationAcceptsPredecessor") {
	constexpr Version generationStart = 100;
	constexpr int8_t remoteTLogLocality = 1;
	LocalityData locality;
	TLogInterface router(locality);
	auto currentSet = makeReference<LogSet>();
	currentSet->locality = remoteTLogLocality;
	currentSet->startVersion = generationStart;
	currentSet->logRouters.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(router)));

	auto logSystem = makeReference<LogSystem>(UID(), locality, LogEpoch(1));
	logSystem->tLogs.push_back(currentSet);
	LogSystemConsumer consumer(logSystem);
	Tag tag(tagLocalityRemoteLog, 0);
	auto routerTag = std::make_pair(router.id(), tag);

	consumer.popLogRouter(generationStart - 2, tag, 0, remoteTLogLocality);
	ASSERT(!logSystem->outstandingPops.contains(routerTag));

	consumer.popLogRouter(generationStart - 1, tag, 0, remoteTLogLocality);
	ASSERT(logSystem->outstandingPops.contains(routerTag));
	ASSERT(logSystem->outstandingPops.at(routerTag).first == generationStart - 1);
	return Void();
}

TEST_CASE("/LogSystem/PeekLogRouter/EmptyOldRangeIsExhausted") {
	LocalityData locality;
	TLogInterface router(locality);
	auto oldSet = makeReference<LogSet>();
	oldSet->logRouters.push_back(
	    makeReference<AsyncVar<OptionalInterface<TLogInterface>>>(OptionalInterface<TLogInterface>(router)));

	auto logSystem = makeReference<LogSystem>(UID(), locality, LogEpoch(1));
	OldLogData old;
	old.epochEnd = 100;
	old.tLogs.push_back(oldSet);
	logSystem->oldLogData.push_back(old);

	LogSystemConsumer consumer(logSystem);
	auto cursor = consumer.peekLogRouter(router.id(), old.epochEnd, Tag(tagLocalityLogRouter, 0), false);
	ASSERT(cursor->isExhausted());
	ASSERT(cursor->version().version == old.epochEnd);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/Simple") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Construct two local tLogs backed by a single LogSet.
	// Both tLogs have known committed version 100 and report a higher durable
	// version 110 that was sent to both log servers.
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	UnknownCommittedVersions ucv(110, 100, std::vector<uint16_t>{ 0, 1 });
	auto logGroupResults = makeLogGroupResults(2, { { ucv }, { ucv } }, { tlogA, tlogB }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "SimpleTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	if (recoverVersion != 110) {
		TraceEvent(SevError, "SimpleTestRecoverVersionFailed").detail("Expected", 110).detail("Got", recoverVersion);
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/FallbackToMaxKCV") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// When no unknown committed versions are reported, should fall back to maxKCV
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	auto logGroupResults = makeLogGroupResults(2, { {}, {} }, { tlogA, tlogB }, true, { 80, 90 });

	Version minDV = 70;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 90) {
		TraceEvent(SevError, "FallbackTestMaxKCVFailed").detail("Expected", 90).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 90);

	if (recoverVersion != 90) {
		TraceEvent(SevError, "FallbackTestRecoverVersionFailed").detail("Expected", 90).detail("Got", recoverVersion);
	}
	ASSERT(recoverVersion == 90);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/HaltOnMissingDelivery") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// When an available tLog didn't receive a version, recovery should halt at the previous version
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	UnknownCommittedVersions ucv(110, 100, std::vector<uint16_t>{ 0, 1 });
	UnknownCommittedVersions ucvLate(120, 110, std::vector<uint16_t>{ 0, 1 });
	// Only tlogA reports the 120 version (tlogB missed it).
	auto logGroupResults = makeLogGroupResults(2, { { ucv, ucvLate }, { ucv } }, { tlogA, tlogB }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "MissingDeliveryTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	// Because not all available tLogs received 120, the recovery version should stay at 110.
	if (recoverVersion != 110) {
		TraceEvent(SevError, "MissingDeliveryTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("Reason", "tlogB did not receive version 120");
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/PolicyNotSatisfied") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// When a version was sent to both tLogs but only received by one (insufficient for RF=2)
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	UnknownCommittedVersions ucv(110, 100, std::vector<uint16_t>{ 0, 1 });
	UnknownCommittedVersions ucv2(120, 110, std::vector<uint16_t>{ 0, 1 });
	// Version 120 was sent to BOTH tLogs but only tlogA received it.
	// With replication factor 2, we need both to receive it.
	auto logGroupResults = makeLogGroupResults(2, { { ucv, ucv2 }, { ucv } }, { tlogA, tlogB }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "PolicyNotSatisfiedTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	if (recoverVersion != 110) {
		TraceEvent(SevError, "PolicyNotSatisfiedTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("Reason", "Version 120 sent to both tLogs but only received by 1 (RF=2)");
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/MinDVRespected") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests that recovery version respects maxKCV when minDV < maxKCV
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	UnknownCommittedVersions ucv(95, 90, std::vector<uint16_t>{ 0, 1 });
	auto logGroupResults = makeLogGroupResults(2, { { ucv }, { ucv } }, { tlogA, tlogB }, true, { 90, 90 });

	Version minDV = 80;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 90) {
		TraceEvent(SevError, "MinDVRespectedTestMaxKCVFailed").detail("Expected", 90).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 90);

	if (recoverVersion != 95) {
		TraceEvent(SevError, "MinDVRespectedTestRecoverVersionFailed")
		    .detail("Expected", 95)
		    .detail("Got", recoverVersion)
		    .detail("MinDV", minDV)
		    .detail("MaxKCV", maxKCV);
	}
	ASSERT(recoverVersion == 95);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/BrokenChain") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests that recovery halts when prevVersion chain is broken
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	// Version 110 and 120 sent to both, but 120's prevVersion != 110 (broken chain)
	UnknownCommittedVersions ucv110(110, 100, std::vector<uint16_t>{ 0, 1 });
	UnknownCommittedVersions ucv120(120, 115, std::vector<uint16_t>{ 0, 1 }); // prevVersion=115, not 110!
	auto logGroupResults =
	    makeLogGroupResults(2, { { ucv110, ucv120 }, { ucv110, ucv120 } }, { tlogA, tlogB }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "BrokenChainTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	// Should stop at 110 because prevVersion chain breaks at 120
	if (recoverVersion != 110) {
		TraceEvent(SevError, "BrokenChainTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("Reason", "Version 120 has prevVersion=115, expected 110");
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/MultipleLogSets") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests recovery with multiple LogSets (primary + satellite)
	LocalityData locality;
	TLogInterface primary1(locality), primary2(locality);
	TLogInterface satellite1(locality), satellite2(locality);

	// Two LogSets: primary (local) + satellite (non-local)
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ primary1, primary2 }, true),
		                                       makeSingleLogSet({ satellite1, satellite2 }, false) };

	// Only the 2 primary tLogs report version 110 (satellite LogSet is non-local and not in logGroupResults)
	UnknownCommittedVersions ucv(110, 100, std::vector<uint16_t>{ 0, 1 });
	auto logGroupResults = makeLogGroupResults(2, { { ucv }, { ucv } }, { primary1, primary2 }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "MultipleLogSetsTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	if (recoverVersion != 110) {
		TraceEvent(SevError, "MultipleLogSetsTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("NumLogSets", logServers.size());
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/PartialAvailabilityPolicyFail") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests that available tLogs must satisfy replication policy
	LocalityData locality;
	TLogInterface tlogA(locality), tlogB(locality), tlogC(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB, tlogC }) };

	// Version 120 sent to all 3, but only 2 of 3 received it (not enough for RF=3)
	UnknownCommittedVersions ucv110(110, 100, std::vector<uint16_t>{ 0, 1, 2 });
	UnknownCommittedVersions ucv120(120, 110, std::vector<uint16_t>{ 0, 1, 2 });
	// Only tlogA and tlogB report receiving 120
	auto logGroupResults = makeLogGroupResults(
	    3, { { ucv110, ucv120 }, { ucv110, ucv120 }, { ucv110 } }, { tlogA, tlogB, tlogC }, false, { 100, 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "PartialAvailabilityTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	// Should stay at 110 because 120 doesn't satisfy RF=3
	if (recoverVersion != 110) {
		TraceEvent(SevError, "PartialAvailabilityTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("Reason", "Only 2 of 3 tLogs received version 120 (RF=3)");
	}
	ASSERT(recoverVersion == 110);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/VersionsBelowMaxKCV") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests that versions <= maxKCV are filtered out
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB }) };

	// Report version 80 (below maxKCV=100), should be ignored
	UnknownCommittedVersions ucv80(80, 70, std::vector<uint16_t>{ 0, 1 });
	auto logGroupResults = makeLogGroupResults(2, { { ucv80 }, { ucv80 } }, { tlogA, tlogB }, true, { 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "VersionsBelowMaxKCVTestMaxKCVFailed").detail("Expected", 100).detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	// Should fall back to maxKCV since all UCVs are <= maxKCV
	if (recoverVersion != 100) {
		TraceEvent(SevError, "VersionsBelowMaxKCVTestRecoverVersionFailed")
		    .detail("Expected", 100)
		    .detail("Got", recoverVersion)
		    .detail("Reason", "All UCVs below maxKCV should be filtered");
	}
	ASSERT(recoverVersion == 100);
	return Void();
}

TEST_CASE("/LogSystem/GetRecoverVersionUnicast/RandomVersionsPartialDelivery") {
	if (!SERVER_KNOBS->ENABLE_VERSION_VECTOR_TLOG_UNICAST) {
		return Void();
	}

	// Tests that recovery handles random versions in range (maxKCV, highest_version) that:
	// 1. Are not reported at all (missing from UCVs)
	// 2. Are only received by a subset of logs (partial delivery)
	LocalityData locality;
	TLogInterface tlogA(locality);
	TLogInterface tlogB(locality);
	TLogInterface tlogC(locality);
	TLogInterface tlogD(locality);
	std::vector<Reference<LogSet>> logServers{ makeSingleLogSet({ tlogA, tlogB, tlogC, tlogD }) };

	// Setup: maxKCV=100, potential versions 110, 112, 115, 118, 120, 125
	// Version 110: received by tlogA and tlogB (full delivery)
	// Version 112: sent to tlogC and tlogD, but NOT REPORTED at all (missing from UCVs)
	// Version 115: only received by tlogA (partial delivery - indicated by tLogLocIds={0})
	// Version 118: sent to tlogC and tlogD, but NOT REPORTED at all (missing from UCVs)
	// Version 120: received by tlogA and tlogB (full delivery)
	// Version 125: only received by tlogA (partial delivery - indicated by tLogLocIds={0})

	UnknownCommittedVersions ucv110(110, 100, std::vector<uint16_t>{ 0, 1 });
	// Version 112 is missing - not in any UCV list
	UnknownCommittedVersions ucv115(115, 112, std::vector<uint16_t>{ 0 }); // Only tlogA (loc 0)
	// Version 118 is missing - not in any UCV list
	UnknownCommittedVersions ucv120(120, 118, std::vector<uint16_t>{ 0, 1 });
	UnknownCommittedVersions ucv125(125, 120, std::vector<uint16_t>{ 0 }); // Only tlogA (loc 0)

	// tlogA reports versions 110, 115, 120, 125
	// tlogB only reports versions 110, 120 (missing 115, 125)
	// tlogC doesn't report any versions
	// tlogD doesn't report any versions
	auto logGroupResults = makeLogGroupResults(2,
	                                           { { ucv110, ucv115, ucv120, ucv125 }, { ucv110, ucv120 }, {}, {} },
	                                           { tlogA, tlogB, tlogC, tlogD },
	                                           true,
	                                           { 100, 100, 100, 100 });

	Version minDV = 90;
	Optional<std::tuple<Version, Version>> result = getRecoverVersionUnicast(logServers, logGroupResults, minDV);
	ASSERT(result.present());
	Version maxKCV = std::get<0>(result.get());
	Version recoverVersion = std::get<1>(result.get());

	if (maxKCV != 100) {
		TraceEvent(SevError, "RandomVersionsPartialDeliveryTestMaxKCVFailed")
		    .detail("Expected", 100)
		    .detail("Got", maxKCV);
	}
	ASSERT(maxKCV == 100);

	// Recovery should stop at 110 because:
	// - 110 was received by tlogA and tlogB (satisfies RF=2)
	// - 112 was not received by tlogC and tlogD
	// - 115 was only received by tlogA (tLogLocIds={0}, doesn't satisfy RF=2)
	// Even though 120 was received by both tlogA and tlogB, the prevVersion chain requires 112, 115, and 118
	if (recoverVersion != 110) {
		TraceEvent(SevError, "RandomVersionsPartialDeliveryTestRecoverVersionFailed")
		    .detail("Expected", 110)
		    .detail("Got", recoverVersion)
		    .detail("Reason",
		            "Missing versions 112 and 118, break recovery before 120. "
		            "Version 115 received by only tlogA (subset), also breaks recovery before 120. ");
	}
	ASSERT(recoverVersion == 110);
	return Void();
}
