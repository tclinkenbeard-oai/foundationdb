/*
 * PhysicalShardCollection.h
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

#pragma once
#ifndef FDBSERVER_DATADISTRIBUTOR_PHYSICAL_SHARD_COLLECTION_H
#define FDBSERVER_DATADISTRIBUTOR_PHYSICAL_SHARD_COLLECTION_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbserver/core/ShardMetrics.h"
#include "fdbserver/datadistributor/DDTxnProcessor.h"
#include "fdbserver/datadistributor/ShardsAffectedByTeamFailure.h"
#include "flow/BooleanParam.h"
#include "flow/FastRef.h"
#include "flow/Trace.h"
#include "flow/flow.h"

// PhysicalShardCollection maintains physical shard concepts in data distribution
// A physical shard contains one or multiple shards (key range)
// PhysicalShardCollection is responsible for creation and maintenance of physical shards (including metrics)
// For multiple DCs, PhysicalShardCollection maintains a pair of primary team and remote team
// A primary team and a remote team shares a physical shard
// For each shard (key-range) move, PhysicalShardCollection decides which physical shard and corresponding team(s) to
// move The current design of PhysicalShardCollection assumes that there exists at most two teamCollections
FDB_BOOLEAN_PARAM(InAnonymousPhysicalShard);
FDB_BOOLEAN_PARAM(PhysicalShardHasMoreThanKeyRange);
FDB_BOOLEAN_PARAM(InOverSizePhysicalShard);
FDB_BOOLEAN_PARAM(PhysicalShardAvailable);
FDB_BOOLEAN_PARAM(MoveKeyRangeOutPhysicalShard);

class PhysicalShardCollection : public ReferenceCounted<PhysicalShardCollection> {
public:
	PhysicalShardCollection() : lastTransitionStartTime(now()), requireTransition(false) {}
	explicit(false) PhysicalShardCollection(Reference<IDDTxnProcessor> db)
	  : txnProcessor(db), lastTransitionStartTime(now()), requireTransition(false) {}

	enum class PhysicalShardCreationTime { DDInit, DDRelocator };

	struct PhysicalShard {
		PhysicalShard() : id(UID().first()) {}

		PhysicalShard(Reference<IDDTxnProcessor> txnProcessor,
		              uint64_t id,
		              StorageMetrics const& metrics,
		              std::vector<ShardsAffectedByTeamFailure::Team> teams,
		              PhysicalShardCreationTime whenCreated)
		  : txnProcessor(txnProcessor), id(id), metrics(metrics),
		    stats(makeReference<AsyncVar<Optional<StorageMetrics>>>()), teams(teams), whenCreated(whenCreated) {}

		// Adds `newRange` to this physical shard and starts monitoring the shard.
		void addRange(const KeyRange& newRange);

		// Removes `outRange` from this physical shard and updates monitored shards.
		void removeRange(const KeyRange& outRange);

		std::string toString() const { return fmt::format("{}", std::to_string(id)); }

		Reference<IDDTxnProcessor> txnProcessor;
		uint64_t id; // physical shard id (never changed)
		StorageMetrics metrics; // current metrics, updated by shardTracker
		// todo(zhewu): combine above metrics with stats. They are redundant.
		Reference<AsyncVar<Optional<StorageMetrics>>> stats; // Stats of this physical shard.
		std::vector<ShardsAffectedByTeamFailure::Team> teams; // which team owns this physical shard (never changed)
		PhysicalShardCreationTime whenCreated; // when this physical shard is created (never changed)

		struct RangeData {
			Future<Void> trackMetrics;
			Reference<AsyncVar<Optional<ShardMetrics>>> stats;
		};
		std::unordered_map<KeyRange, RangeData> rangeData;

	private:
		// Inserts a new key range into this physical shard. `newRange` must not exist in this shard already.
		void insertNewRangeData(const KeyRange& newRange);
	};

	// Generate a random physical shard ID, which is not UID().first() nor anonymousShardId.first()
	uint64_t generateNewPhysicalShardID(uint64_t debugID);

	// If the input team has any available physical shard, return an available physical shard from the input team and
	// not in `excludedPhysicalShards`. This method is used for two-step team selection The overall process has two
	// steps: Step 1: get a physical shard id given the input primary team Return a new physical shard id if the input
	// primary team is new or the team has no available physical shard checkPhysicalShardAvailable() defines whether a
	// physical shard is available
	Optional<uint64_t> trySelectAvailablePhysicalShardFor(ShardsAffectedByTeamFailure::Team team,
	                                                      StorageMetrics const& metrics,
	                                                      const std::unordered_set<uint64_t>& excludedPhysicalShards,
	                                                      uint64_t debugID);

	// Step 2: get a remote team which has the input physical shard.
	// Second field in the returned pair indicates whether this physical shard is available or not.
	// Return empty if no such remote team.
	// May return a problematic remote team, and re-selection is required for this case.
	std::pair<Optional<ShardsAffectedByTeamFailure::Team>, bool>
	tryGetAvailableRemoteTeamWith(uint64_t inputPhysicalShardID, StorageMetrics const& moveInMetrics, uint64_t debugID);
	// Invariant:
	// (1) If forceToUseNewPhysicalShard is set, use the bestTeams selected by getTeam(), and create a new physical
	// shard for the teams
	// (2) If forceToUseNewPhysicalShard is not set, use the primary team selected by getTeam()
	//     If there exists a remote team which has an available physical shard with the primary team
	//         Then, use the remote team. Note that the remote team may be unhealthy and the remote team
	//         may be one who issues the current data relocation.
	//         In this case, we set forceToUseNewPhysicalShard to use getTeam() to re-select the remote team
	//     Otherwise, use getTeam() to re-select the remote team

	// Create a physical shard when initializing PhysicalShardCollection
	void initPhysicalShardCollection(KeyRange keys,
	                                 std::vector<ShardsAffectedByTeamFailure::Team> selectedTeams,
	                                 uint64_t physicalShardID,
	                                 uint64_t debugID);

	// Create a physical shard when updating PhysicalShardCollection
	void updatePhysicalShardCollection(KeyRange keys,
	                                   bool isRestore,
	                                   std::vector<ShardsAffectedByTeamFailure::Team> selectedTeams,
	                                   uint64_t physicalShardID,
	                                   const StorageMetrics& metrics,
	                                   uint64_t debugID);

	// Update physicalShard metrics and return whether the keyRange needs to move out of its physical shard
	MoveKeyRangeOutPhysicalShard trackPhysicalShard(KeyRange keyRange,
	                                                StorageMetrics const& newMetrics,
	                                                StorageMetrics const& oldMetrics,
	                                                bool initWithNewMetrics);

	// Clean up empty physicalShard
	void cleanUpPhysicalShardCollection();

	// Log physicalShard
	void logPhysicalShardCollection();

	// Checks if a physical shard exists.
	bool physicalShardExists(uint64_t physicalShardID);

private:
	// Track physicalShard metrics by tracking keyRange metrics
	void updatePhysicalShardMetricsByKeyRange(KeyRange keyRange,
	                                          StorageMetrics const& newMetrics,
	                                          StorageMetrics const& oldMetrics,
	                                          bool initWithNewMetrics);

	// Check the input keyRange is in the anonymous physical shard
	InAnonymousPhysicalShard isInAnonymousPhysicalShard(KeyRange keyRange);

	// Check the input physicalShard has more keyRanges in addition to the input keyRange
	PhysicalShardHasMoreThanKeyRange whetherPhysicalShardHasMoreThanKeyRange(uint64_t physicalShardID,
	                                                                         KeyRange keyRange);

	// Check the input keyRange is in an oversize physical shard
	// This function returns true to enforce the keyRange to move out the physical shard
	// Note that if the physical shard only contains the keyRange, always return FALSE
	InOverSizePhysicalShard isInOverSizePhysicalShard(KeyRange keyRange);

	// Check whether the input physical shard is available
	// A physical shard is available if the current metric + moveInMetrics <= a threshold
	PhysicalShardAvailable checkPhysicalShardAvailable(uint64_t physicalShardID, StorageMetrics const& moveInMetrics);

	// Reduce the metrics of input physical shard by the input metrics
	void reduceMetricsForMoveOut(uint64_t physicalShardID, StorageMetrics const& metrics);

	// Add the input metrics to the metrics of input physical shard
	void increaseMetricsForMoveIn(uint64_t physicalShardID, StorageMetrics const& metrics);

	// In physicalShardCollection, add a physical shard initialized by the input parameters to the collection
	void insertPhysicalShardToCollection(uint64_t physicalShardID,
	                                     StorageMetrics const& metrics,
	                                     std::vector<ShardsAffectedByTeamFailure::Team> teams,
	                                     uint64_t debugID,
	                                     PhysicalShardCreationTime whenCreated);

	// In teamPhysicalShardIDs, add the input physical shard id to the input teams
	void updateTeamPhysicalShardIDsMap(uint64_t physicalShardID,
	                                   std::vector<ShardsAffectedByTeamFailure::Team> inputTeams,
	                                   uint64_t debugID);

	// In keyRangePhysicalShardIDMap, set the input physical shard id to the input key range
	void updatekeyRangePhysicalShardIDMap(KeyRange keyRange, uint64_t physicalShardID, uint64_t debugID);

	// Checks the consistency between the mapping of physical shards and key ranges.
	void checkKeyRangePhysicalShardMapping();

	// Return a string concatenating the input IDs interleaving with " "
	std::string convertIDsToString(std::set<uint64_t> ids);

	// Reset TransitionStartTime
	// Consider a system without concept of physicalShard
	// When restart, the system begins with a state where all keyRanges are in the anonymousShard
	// Our goal is to make all keyRanges are out of the anonymousShard
	// A keyRange moves out of the anonymousShard when the keyRange is triggered a data move
	// It is possible that a keyRange is cold and no data move is triggered on this keyRange for long time
	// In this case, we need to intentionally trigger data move on that keyRange
	// The minimal time span between two successive data move for this purpose is TransitionStartTime
	inline void resetLastTransitionStartTime() { // reset when a keyRange move is triggered for the transition
		lastTransitionStartTime = now();
		return;
	}

	// When DD restarts, it checks whether keyRange has anonymousShard
	// If yes, setTransitionCheck() is call to trigger the process of removing anonymousShard
	inline void setTransitionCheck() {
		if (requireTransition) {
			return;
		}
		requireTransition = true;
		TraceEvent("PhysicalShardSetTransitionCheck");
		return;
	}

	inline bool requireTransitionCheck() { return requireTransition; }

	Reference<IDDTxnProcessor> txnProcessor;

	// Core data structures
	// Physical shard instances indexed by physical shard id
	std::unordered_map<uint64_t, PhysicalShard> physicalShardInstances;
	// Indicate a key range belongs to which physical shard
	KeyRangeMap<uint64_t> keyRangePhysicalShardIDMap;
	// Indicate what physical shards owned by a team
	std::map<ShardsAffectedByTeamFailure::Team, std::set<uint64_t>> teamPhysicalShardIDs;
	bool requireTransition;
	double lastTransitionStartTime;
};

#endif // FDBSERVER_DATADISTRIBUTOR_PHYSICAL_SHARD_COLLECTION_H
