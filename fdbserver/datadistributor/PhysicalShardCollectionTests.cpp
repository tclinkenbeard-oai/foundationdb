/*
 * PhysicalShardCollectionTests.cpp
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

#include "fdbserver/datadistributor/PhysicalShardCollection.h"

#include <unordered_set>

#include "fdbserver/core/Knobs.h"
#include "flow/UnitTest.h"

void forceLinkPhysicalShardCollectionTests() {}

namespace {

struct PhysicalShardKnobGuard {
	ServerKnobs& knobs = *const_cast<ServerKnobs*>(SERVER_KNOBS);
	bool oldShardEncodeLocationMetadata = knobs.SHARD_ENCODE_LOCATION_METADATA;
	bool oldEnablePhysicalShard = knobs.ENABLE_DD_PHYSICAL_SHARD;
	int64_t oldMaxPhysicalShardBytes = knobs.MAX_PHYSICAL_SHARD_BYTES;

	PhysicalShardKnobGuard() {
		knobs.SHARD_ENCODE_LOCATION_METADATA = true;
		knobs.ENABLE_DD_PHYSICAL_SHARD = true;
	}

	~PhysicalShardKnobGuard() {
		knobs.SHARD_ENCODE_LOCATION_METADATA = oldShardEncodeLocationMetadata;
		knobs.ENABLE_DD_PHYSICAL_SHARD = oldEnablePhysicalShard;
		knobs.MAX_PHYSICAL_SHARD_BYTES = oldMaxPhysicalShardBytes;
	}
};

std::vector<ShardsAffectedByTeamFailure::Team> makeTeams(int firstServerID) {
	return { ShardsAffectedByTeamFailure::Team({ UID(firstServerID, 0), UID(firstServerID + 1, 0) }, true),
		     ShardsAffectedByTeamFailure::Team({ UID(firstServerID + 2, 0), UID(firstServerID + 3, 0) }, false) };
}

} // namespace

TEST_CASE("/DataDistribution/PhysicalShardCollection/RangeOwnershipTransition") {
	PhysicalShardKnobGuard knobGuard;
	PhysicalShardCollection collection;
	const auto oldTeams = makeTeams(1);
	const auto newTeams = makeTeams(5);
	const uint64_t oldPhysicalShardID = 101;
	const uint64_t newPhysicalShardID = 202;
	const KeyRange wholeRange = Standalone(KeyRangeRef("a"_sr, "z"_sr));
	const KeyRange leftRange = Standalone(KeyRangeRef("a"_sr, "m"_sr));
	const KeyRange rightRange = Standalone(KeyRangeRef("m"_sr, "z"_sr));

	collection.initPhysicalShardCollection(wholeRange, oldTeams, oldPhysicalShardID, 0);
	ASSERT(collection.physicalShardExists(oldPhysicalShardID));

	collection.updatePhysicalShardCollection(rightRange, false, newTeams, newPhysicalShardID, StorageMetrics(), 0);
	collection.cleanUpPhysicalShardCollection();
	ASSERT(collection.physicalShardExists(oldPhysicalShardID));
	ASSERT(collection.physicalShardExists(newPhysicalShardID));

	auto oldChoice = collection.trySelectAvailablePhysicalShardFor(
	    oldTeams.front(), StorageMetrics(), std::unordered_set<uint64_t>(), 0);
	ASSERT(oldChoice.present());
	ASSERT(oldChoice.get() == oldPhysicalShardID);

	auto newChoice = collection.trySelectAvailablePhysicalShardFor(
	    newTeams.front(), StorageMetrics(), std::unordered_set<uint64_t>(), 0);
	ASSERT(newChoice.present());
	ASSERT(newChoice.get() == newPhysicalShardID);

	collection.updatePhysicalShardCollection(leftRange, false, newTeams, newPhysicalShardID, StorageMetrics(), 0);
	collection.cleanUpPhysicalShardCollection();
	ASSERT(!collection.physicalShardExists(oldPhysicalShardID));
	ASSERT(collection.physicalShardExists(newPhysicalShardID));
	ASSERT(
	    !collection
	         .trySelectAvailablePhysicalShardFor(oldTeams.front(), StorageMetrics(), std::unordered_set<uint64_t>(), 0)
	         .present());

	auto remoteTeam = collection.tryGetAvailableRemoteTeamWith(newPhysicalShardID, StorageMetrics(), 0);
	ASSERT(remoteTeam.second);
	ASSERT(remoteTeam.first.present());
	ASSERT(remoteTeam.first.get() == newTeams[1]);

	return Void();
}

TEST_CASE("/DataDistribution/PhysicalShardCollection/OversizedShardTriggersMoveOut") {
	PhysicalShardKnobGuard knobGuard;
	knobGuard.knobs.MAX_PHYSICAL_SHARD_BYTES = 100;

	PhysicalShardCollection collection;
	const auto teams = makeTeams(1);
	const uint64_t physicalShardID = 101;
	const KeyRange leftRange = Standalone(KeyRangeRef("a"_sr, "m"_sr));
	const KeyRange rightRange = Standalone(KeyRangeRef("m"_sr, "z"_sr));

	collection.initPhysicalShardCollection(leftRange, teams, physicalShardID, 0);
	collection.initPhysicalShardCollection(rightRange, teams, physicalShardID, 0);

	StorageMetrics oversizedMetrics;
	oversizedMetrics.bytes = knobGuard.knobs.MAX_PHYSICAL_SHARD_BYTES + 1;
	ASSERT(collection.trackPhysicalShard(leftRange, oversizedMetrics, StorageMetrics(), true));

	return Void();
}
