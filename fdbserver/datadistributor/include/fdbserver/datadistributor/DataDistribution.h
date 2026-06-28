/*
 * DataDistribution.h
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
#ifndef FDBSERVER_DATADISTRIBUTOR_DATA_DISTRIBUTION_H
#define FDBSERVER_DATADISTRIBUTOR_DATA_DISTRIBUTION_H

#include "fdbclient/BulkLoading.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/core/DataDistributorInterface.h"
#include "fdbserver/core/Knobs.h"
#include "fdbserver/core/MoveKeys.h"
#include "fdbserver/core/DataMovement.h"
#include "fdbserver/core/ShardMetrics.h"
#include "fdbserver/core/ShardSizing.h"
#include "fdbclient/RunRYWTransaction.h"
#include "fdbserver/datadistributor/DDTxnProcessor.h"
#include "fdbserver/datadistributor/ShardsAffectedByTeamFailure.h"
#include "fdbserver/datadistributor/TCInfo.h"
#include "fdbclient/StorageWiggleMetrics.h"
#include "fdbclient/DataDistributionConfig.h"
#include <boost/heap/policies.hpp>
#include <boost/heap/skew_heap.hpp>

/////////////////////////////// Data //////////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Data
#endif

// SOMEDAY: whether it's possible to combine RelocateReason and DataMovementReason together?
// RelocateReason to DataMovementReason is one-to-N mapping
class RelocateReason {
public:
	enum Value : int8_t {
		OTHER = 0,
		REBALANCE_DISK,
		REBALANCE_READ,
		REBALANCE_WRITE,
		MERGE_SHARD,
		SIZE_SPLIT,
		WRITE_SPLIT,
		__COUNT
	};
	explicit(false) RelocateReason(Value v) : value(v) { ASSERT(value != __COUNT); }
	explicit RelocateReason(int v) : value((Value)v) { ASSERT(value != __COUNT); }
	std::string toString() const {
		switch (value) {
		case OTHER:
			return "Other";
		case REBALANCE_DISK:
			return "RebalanceDisk";
		case REBALANCE_READ:
			return "RebalanceRead";
		case REBALANCE_WRITE:
			return "RebalanceWrite";
		case MERGE_SHARD:
			return "MergeShard";
		case SIZE_SPLIT:
			return "SizeSplit";
		case WRITE_SPLIT:
			return "WriteSplit";
		case __COUNT:
			ASSERT(false);
		}
		return "";
	}
	operator int() const { return (int)value; }
	constexpr static int8_t typeCount() { return (int)__COUNT; }
	bool operator<(const RelocateReason& reason) { return (int)value < (int)reason.value; }

private:
	Value value;
};

DataMoveType getDataMoveTypeFromDataMoveId(const UID& dataMoveId);

struct DDShardInfo;

// Represents a data move in DD.
struct DataMove {
	DataMove() : meta(DataMoveMetaData()), restore(false), valid(false), cancelled(false) {}
	explicit DataMove(DataMoveMetaData meta, bool restore)
	  : meta(std::move(meta)), restore(restore), valid(true), cancelled(meta.getPhase() == DataMoveMetaData::Deleting) {
	}

	// Checks if the DataMove is consistent with the shard.
	void validateShard(const DDShardInfo& shard, KeyRangeRef range, int priority = SERVER_KNOBS->PRIORITY_RECOVER_MOVE);

	bool isCancelled() const { return this->cancelled; }

	const DataMoveMetaData meta;
	bool restore; // The data move is scheduled by a previous DD, and is being recovered now.
	bool valid; // The data move data is integral.
	bool cancelled; // The data move has been cancelled.
	std::vector<UID> primarySrc;
	std::vector<UID> remoteSrc;
	std::vector<UID> primaryDest;
	std::vector<UID> remoteDest;
};

struct RelocateShard {
	KeyRange keys;
	int priority;
	bool cancelled; // The data move should be cancelled.
	std::shared_ptr<DataMove> dataMove; // Not null if this is a restored data move.
	UID dataMoveId;
	RelocateReason reason;
	DataMovementReason moveReason;

	UID traceId; // track the lifetime of this relocate shard

	// Initialization when define is a better practice. We should avoid assignment of member after definition.
	// static RelocateShard emptyRelocateShard() { return {}; }

	RelocateShard(KeyRange const& keys, DataMovementReason moveReason, RelocateReason reason, UID traceId = UID())
	  : keys(keys), priority(dataMovementPriority(moveReason)), cancelled(false), dataMoveId(anonymousShardId),
	    reason(reason), moveReason(moveReason), traceId(traceId) {}

	RelocateShard(KeyRange const& keys, int priority, RelocateReason reason, UID traceId = UID())
	  : keys(keys), priority(priority), cancelled(false), dataMoveId(anonymousShardId), reason(reason),
	    moveReason(priorityToDataMovementReason(priority)), traceId(traceId) {}

	bool isRestore() const { return this->dataMove != nullptr; }

	void setParentRange(KeyRange const& parent);
	Optional<KeyRange> getParentRange() const;

	RelocateShard()
	  : priority(0), cancelled(false), dataMoveId(anonymousShardId), reason(RelocateReason::OTHER),
	    moveReason(DataMovementReason::INVALID) {}

private:
	// If this rs comes from a splitting, parent range is the original range.
	Optional<KeyRange> parent_range;
};

struct GetMetricsRequest {
	KeyRange keys;
	Promise<StorageMetrics> reply;
	GetMetricsRequest() {}
	explicit(false) GetMetricsRequest(KeyRange const& keys) : keys(keys) {}
};

struct GetTopKMetricsReply {
	struct KeyRangeStorageMetrics {
		KeyRange range;
		StorageMetrics metrics;
		KeyRangeStorageMetrics() = default;
		KeyRangeStorageMetrics(const KeyRange& range, const StorageMetrics& s) : range(range), metrics(s) {}
	};
	std::vector<KeyRangeStorageMetrics> shardMetrics;
	double minReadLoad = -1, maxReadLoad = -1;
	GetTopKMetricsReply() {}
	GetTopKMetricsReply(std::vector<KeyRangeStorageMetrics> const& m, double minReadLoad, double maxReadLoad)
	  : shardMetrics(m), minReadLoad(minReadLoad), maxReadLoad(maxReadLoad) {}
};

struct GetTopKMetricsRequest {
private:
	int topK = 1; // default only return the top 1 shard based on the GetTopKMetricsRequest::compare function
public:
	std::vector<KeyRange> keys;
	Promise<GetTopKMetricsReply> reply; // topK storage metrics
	double maxReadLoadPerKSecond = 0, minReadLoadPerKSecond = 0; // all returned shards won't exceed this read load

	GetTopKMetricsRequest() {}
	explicit(false) GetTopKMetricsRequest(std::vector<KeyRange> const& keys,
	                                      int topK = 1,
	                                      double maxReadLoadPerKSecond = std::numeric_limits<double>::max(),
	                                      double minReadLoadPerKSecond = 0)
	  : topK(topK), keys(keys), maxReadLoadPerKSecond(maxReadLoadPerKSecond),
	    minReadLoadPerKSecond(minReadLoadPerKSecond) {
		ASSERT_GE(topK, 1);
	}

	int getTopK() const { return topK; };

	// Return true if a.score > b.score, return the largest topK in keys
	static bool compare(const GetTopKMetricsReply::KeyRangeStorageMetrics& a,
	                    const GetTopKMetricsReply::KeyRangeStorageMetrics& b) {
		return compareByReadDensity(a, b);
	}

private:
	// larger read density means higher score
	static bool compareByReadDensity(const GetTopKMetricsReply::KeyRangeStorageMetrics& a,
	                                 const GetTopKMetricsReply::KeyRangeStorageMetrics& b) {
		return a.metrics.readLoadKSecond() / std::max(a.metrics.bytes * 1.0, 1.0) >
		       b.metrics.readLoadKSecond() / std::max(b.metrics.bytes * 1.0, 1.0);
	}
};

struct GetMetricsListRequest {
	KeyRange keys;
	int shardLimit;
	Promise<Standalone<VectorRef<DDMetricsRef>>> reply;

	GetMetricsListRequest() {}
	GetMetricsListRequest(KeyRange const& keys, const int shardLimit) : keys(keys), shardLimit(shardLimit) {}
};

struct BulkLoadShardRequest {
	BulkLoadTaskState bulkLoadTaskState;
	Optional<int> cancelledDataMovePriority; // Set to the data move priority of the task if the task is failed for
	                                         // unretryable error.
	BulkLoadShardRequest() = default;

	explicit BulkLoadShardRequest(BulkLoadTaskState const& bulkLoadTaskState) : bulkLoadTaskState(bulkLoadTaskState) {}

	BulkLoadShardRequest(BulkLoadTaskState const& bulkLoadTaskState, int cancelledDataMovePriority)
	  : bulkLoadTaskState(bulkLoadTaskState), cancelledDataMovePriority(cancelledDataMovePriority) {}
};

struct RebalanceStorageQueueRequest {
	UID serverId;
	std::vector<ShardsAffectedByTeamFailure::Team> teams;
	bool primary;

	RebalanceStorageQueueRequest() {}
	RebalanceStorageQueueRequest(UID serverId,
	                             const std::vector<ShardsAffectedByTeamFailure::Team>& teams,
	                             bool primary)
	  : serverId(serverId), teams(teams), primary(primary) {}
};

// DDShardInfo is so named to avoid link-time name collision with ShardInfo within the StorageServer
struct DDShardInfo {
	Key key;
	// all UID are sorted
	std::vector<UID> primarySrc;
	std::vector<UID> remoteSrc;
	std::vector<UID> primaryDest;
	std::vector<UID> remoteDest;
	bool hasDest;
	UID srcId;
	UID destId;

	explicit DDShardInfo(Key key) : key(key), hasDest(false) {}
	DDShardInfo(Key key, UID srcId, UID destId) : key(key), hasDest(false), srcId(srcId), destId(destId) {}
};

struct InitialDataDistribution : ReferenceCounted<InitialDataDistribution> {
	InitialDataDistribution()
	  : dataMoveMap(std::make_shared<DataMove>()),
	    userRangeConfig(makeReference<DDConfiguration::RangeConfigMapSnapshot>(allKeys.begin, allKeys.end)) {}

	// Read from dataDistributionModeKey. Whether DD is disabled. DD can be disabled persistently (mode = 0). Set mode
	// to 1 will enable all disabled parts
	int mode;
	int bulkLoadMode = 0;
	int bulkDumpMode = 0;
	std::vector<std::pair<StorageServerInterface, ProcessClass>> allServers;
	std::set<std::vector<UID>> primaryTeams;
	std::set<std::vector<UID>> remoteTeams;
	std::vector<DDShardInfo> shards;
	std::vector<UID> toCleanDataMoveTombstone;
	Optional<Key> initHealthyZoneValue; // set for maintenance mode
	KeyRangeMap<std::shared_ptr<DataMove>> dataMoveMap;
	std::vector<AuditStorageState> auditStates;
	Reference<DDConfiguration::RangeConfigMapSnapshot> userRangeConfig;
};

struct TeamCollectionInterface {
	PromiseStream<GetTeamRequest> getTeam;
};

// Used to track the number of ongoing bulkload tasks for each storage server
struct DDBulkLoadTaskBusyMap {
public:
	void addTask(const UID& ssid) { busyMap[ssid]++; }

	void removeTask(const UID& ssid) {
		auto it = busyMap.find(ssid);
		ASSERT(it != busyMap.end());
		it->second--;
		if (it->second == 0) {
			busyMap.erase(it);
		}
	}

	int getTaskCount(const UID& ssid) {
		auto it = busyMap.find(ssid);
		if (it == busyMap.end()) {
			return 0;
		} else {
			return it->second;
		}
	}

private:
	std::unordered_map<UID, int> busyMap; // <Storage Server ID, Task Count>
};

// Used to piggyback the data move priority when an unretryable error happens to the task datamove.
// If the priority indicates the data move is a team unhealthy related data move, the bulkload engine
// system trigger a new data move when terminate the error task.
struct BulkLoadAck {
	bool unretryableError = false;
	int dataMovePriority = -1;

	BulkLoadAck() = default;
	BulkLoadAck(bool unretryableError, int dataMovePriority)
	  : unretryableError(unretryableError), dataMovePriority(dataMovePriority) {}
};

struct DDBulkLoadEngineTask {
	BulkLoadTaskState coreState;
	Version commitVersion = invalidVersion;
	Promise<BulkLoadAck> completeAck; // Satisfied when a data move for this task completes or unretryable error for
	                                  // the first time, where the task metadata phase is Complete or Error.

	DDBulkLoadEngineTask() = default;

	DDBulkLoadEngineTask(BulkLoadTaskState coreState, Version commitVersion, Promise<BulkLoadAck> completeAck)
	  : coreState(coreState), commitVersion(commitVersion), completeAck(completeAck) {}

	bool operator==(const DDBulkLoadEngineTask& rhs) const {
		return coreState == rhs.coreState && commitVersion == rhs.commitVersion;
	}

	std::string toString() const {
		return coreState.toString() + ", [CommitVersion]: " + std::to_string(commitVersion);
	}
};

inline bool bulkLoadIsEnabled(int bulkLoadModeValue) {
	return SERVER_KNOBS->SHARD_ENCODE_LOCATION_METADATA && bulkLoadModeValue == 1;
}

inline bool bulkDumpIsEnabled(int bulkDumpModeValue) {
	return bulkDumpModeValue == 1;
}

class BulkLoadTaskCollection : public ReferenceCounted<BulkLoadTaskCollection> {
public:
	explicit(false) BulkLoadTaskCollection(UID ddId) : ddId(ddId) {
		bulkLoadTaskMap.insert(allKeys, Optional<DDBulkLoadEngineTask>());
	}

	// Return true if there exists a bulk load job/task or the collection has not been initialized.
	// This takes effect only when DDBulkLoad Mode is enabled.
	bool bulkLoading(const KeyRange& range) {
		if (!initialized) {
			return true;
		}
		if (bulkLoadJobRange.present()) {
			KeyRange jobOverlap = bulkLoadJobRange.get() & range;
			if (!jobOverlap.empty()) {
				return true;
			}
		}
		for (auto it : bulkLoadTaskMap.intersectingRanges(range)) {
			if (!it->value().present()) {
				continue;
			}
			return true;
		}
		return false;
	}

	void setBulkLoadJobRange(const KeyRange& range) {
		bulkLoadJobRange = range;
		initialized = true;
		return;
	}

	void removeBulkLoadJobRange() {
		bulkLoadJobRange.reset();
		initialized = true;
		return;
	}

	// Return true if there exists a bulk load task since the given commit version
	bool overlappingTaskSince(KeyRange range, Version sinceCommitVersion) {
		for (auto it : bulkLoadTaskMap.intersectingRanges(range)) {
			if (!it->value().present()) {
				continue;
			}
			if (it->value().get().commitVersion > sinceCommitVersion) {
				return true;
			}
		}
		return false;
	}

	// Add a task and this task becomes visible to DDTracker and DDQueue
	// DDTracker stops any shard boundary change overlapping the task range
	// DDQueue attaches the task to following data moves until the task has been completed
	// If there are overlapped old tasks, make it outdated by sending a signal to completeAck
	void publishTask(const BulkLoadTaskState& bulkLoadTaskState,
	                 Version commitVersion,
	                 Promise<BulkLoadAck> completeAck) {
		if (overlappingTaskSince(bulkLoadTaskState.getRange(), commitVersion)) {
			throw bulkload_task_outdated();
		}
		DDBulkLoadEngineTask task(bulkLoadTaskState, commitVersion, completeAck);
		TraceEvent(SevDebug, "DDBulkLoadTaskCollectionPublishTask", ddId)
		    .setMaxEventLength(-1)
		    .setMaxFieldLength(-1)
		    .detail("Range", bulkLoadTaskState.getRange())
		    .detail("Task", task.toString());
		// For any overlapping task, make it outdated
		for (auto it : bulkLoadTaskMap.intersectingRanges(bulkLoadTaskState.getRange())) {
			if (!it->value().present()) {
				continue;
			}
			if (it->value().get().coreState.getTaskId() == bulkLoadTaskState.getTaskId()) {
				ASSERT(it->value().get().coreState.getRange() == bulkLoadTaskState.getRange());
				// In case that the task has been already triggered
				// Avoid repeatedly being triggered by throwing the error
				// then the current doBulkLoadTask will sliently exit
				throw bulkload_task_outdated();
			}
			if (it->value().get().completeAck.canBeSet()) {
				it->value().get().completeAck.sendError(bulkload_task_outdated());
				TraceEvent(bulkLoadVerboseEventSev(), "DDBulkLoadTaskCollectionPublishTaskOverwriteTask", ddId)
				    .detail("NewTaskRange", bulkLoadTaskState.getRange())
				    .detail("NewJobId", task.coreState.getJobId())
				    .detail("NewTaskId", task.coreState.getTaskId())
				    .detail("NewCommitVersion", task.commitVersion)
				    .detail("OldTaskRange", it->range())
				    .detail("OldJobId", it->value().get().coreState.getJobId())
				    .detail("OldTaskId", it->value().get().coreState.getTaskId())
				    .detail("OldCommitVersion", it->value().get().commitVersion);
			}
		}
		bulkLoadTaskMap.insert(bulkLoadTaskState.getRange(), task);
		return;
	}

	// This method is called when there is a data move assigned to run the bulk load task
	void startTask(const BulkLoadTaskState& bulkLoadTaskState) {
		for (auto it : bulkLoadTaskMap.intersectingRanges(bulkLoadTaskState.getRange())) {
			if (!it->value().present() || it->value().get().coreState.getTaskId() != bulkLoadTaskState.getTaskId()) {
				throw bulkload_task_outdated();
			}
			TraceEvent(SevDebug, "DDBulkLoadTaskCollectionStartTask", ddId)
			    .detail("Range", bulkLoadTaskState.getRange())
			    .detail("TaskRange", it->range())
			    .detail("Task", it->value().get().toString());
		}
		return;
	}

	// Send complete signal to indicate this task has been completed
	void terminateTask(const BulkLoadTaskState& bulkLoadTaskState) {
		for (auto it : bulkLoadTaskMap.intersectingRanges(bulkLoadTaskState.getRange())) {
			if (!it->value().present() || it->value().get().coreState.getTaskId() != bulkLoadTaskState.getTaskId()) {
				throw bulkload_task_outdated();
			}
			// It is possible that the task has been completed by a past data move
			if (it->value().get().completeAck.canBeSet()) {
				it->value().get().completeAck.send(BulkLoadAck());
				TraceEvent(SevDebug, "DDBulkLoadTaskCollectionTerminateTask", ddId)
				    .detail("Range", bulkLoadTaskState.getRange())
				    .detail("TaskRange", it->range())
				    .detail("Task", it->value().get().toString());
			}
		}
		return;
	}

	// Erase any metadata on the map for the input bulkload task
	void eraseTask(const BulkLoadTaskState& bulkLoadTaskState) {
		std::vector<KeyRange> rangesToClear;
		for (auto it : bulkLoadTaskMap.intersectingRanges(bulkLoadTaskState.getRange())) {
			if (!it->value().present() || it->value().get().coreState.getTaskId() != bulkLoadTaskState.getTaskId()) {
				continue;
			}
			TraceEvent(SevDebug, "DDBulkLoadTaskCollectionEraseTaskdata", ddId)
			    .detail("Range", bulkLoadTaskState.getRange())
			    .detail("TaskRange", it->range())
			    .detail("Task", it->value().get().toString());
			rangesToClear.push_back(it->range());
		}
		for (const auto& rangeToClear : rangesToClear) {
			bulkLoadTaskMap.insert(rangeToClear, Optional<DDBulkLoadEngineTask>());
		}
		bulkLoadTaskMap.coalesce(normalKeys);
		return;
	}

	// Get the task which has exactly the same range as the input range
	Optional<DDBulkLoadEngineTask> getTaskByRange(KeyRange range) const {
		Optional<DDBulkLoadEngineTask> res;
		for (auto it : bulkLoadTaskMap.intersectingRanges(range)) {
			if (!it->value().present()) {
				continue;
			}
			DDBulkLoadEngineTask bulkLoadTask = it->value().get();
			TraceEvent(SevDebug, "DDBulkLoadTaskCollectionGetPublishedTaskEach", ddId)
			    .detail("Range", range)
			    .detail("TaskRange", it->range())
			    .detail("Task", bulkLoadTask.toString());
			if (bulkLoadTask.coreState.getRange() == range) {
				ASSERT(!res.present());
				res = bulkLoadTask;
			}
		}
		TraceEvent(SevDebug, "DDBulkLoadTaskCollectionGetPublishedTask", ddId)
		    .detail("Range", range)
		    .detail("Task", res.present() ? describe(res.get()) : "");
		return res;
	}

	DDBulkLoadTaskBusyMap busyMap; // <SSID, taskCount>

private:
	KeyRangeMap<Optional<DDBulkLoadEngineTask>> bulkLoadTaskMap;
	UID ddId;
	Optional<KeyRange> bulkLoadJobRange;
	bool initialized = false;
};

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

/////////////////////////////// Perpetual Storage Wiggle //////////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Perpetual Storage Wiggle
#endif
struct DDTeamCollectionInitParams;
class DDTeamCollection;

struct StorageWiggler : ReferenceCounted<StorageWiggler> {
	static constexpr double MIN_ON_CHECK_DELAY_SEC = 5.0;
	using State = StorageWigglerState::Value;
	static constexpr State INVALID = StorageWigglerState::INVALID;
	static constexpr State RUN = StorageWigglerState::RUN;
	static constexpr State PAUSE = StorageWigglerState::PAUSE;

	DDTeamCollection const* teamCollection;
	StorageWiggleData wiggleData; // the wiggle related data persistent in database

	StorageWiggleMetrics metrics;
	AsyncVar<bool> stopWiggleSignal;
	// data structures
	typedef std::pair<StorageMetadataType, UID> MetadataUIDP;
	// min-heap
	boost::heap::skew_heap<MetadataUIDP, boost::heap::mutable_<true>, boost::heap::compare<std::greater<MetadataUIDP>>>
	    wiggle_pq;
	std::unordered_map<UID, decltype(wiggle_pq)::handle_type> pq_handles;

	State wiggleState = INVALID;
	double lastStateChangeTs = 0.0; // timestamp describes when did the state change

	explicit StorageWiggler(DDTeamCollection* collection) : teamCollection(collection), stopWiggleSignal(true) {};
	// wiggle related actors will quit when this signal is set to true
	void setStopSignal(bool value) { stopWiggleSignal.set(value); }
	bool isStopped() const { return stopWiggleSignal.get(); }
	// add server to wiggling queue
	void addServer(const UID& serverId, const StorageMetadataType& metadata);
	// remove server from wiggling queue
	void removeServer(const UID& serverId);
	// update metadata and adjust priority_queue
	void updateMetadata(const UID& serverId, const StorageMetadataType& metadata);
	bool contains(const UID& serverId) const { return pq_handles.contains(serverId); }
	bool empty() const { return wiggle_pq.empty(); }

	// It's guarantee that When a.metadata >= b.metadata, if !necessary(a) then !necessary(b)
	bool necessary(const UID& serverId, const StorageMetadataType& metadata) const;

	// try to return the next storage server that is necessary to wiggle
	Optional<UID> getNextServerId(bool necessaryOnly = true);
	// next check time to avoid busy loop
	Future<Void> onCheck() const;
	State getWiggleState() const { return wiggleState; }
	void setWiggleState(State s) {
		if (wiggleState != s) {
			wiggleState = s;
			lastStateChangeTs = g_network->now();
		}
	}
	static std::string getWiggleStateStr(State s) { return StorageWigglerState::toString(s); }

	// -- statistic update

	// reset Statistic in database when perpetual wiggle is closed by user
	Future<Void> resetStats();
	// restore Statistic from database when the perpetual wiggle is opened
	Future<Void> restoreStats();
	// called when start wiggling a SS
	Future<Void> startWiggle();
	Future<Void> finishWiggle();
	bool shouldStartNewRound() const { return metrics.last_round_finish >= metrics.last_round_start; }
	bool shouldFinishRound() const {
		if (wiggle_pq.empty())
			return true;
		return (wiggle_pq.top().first.createdTime >= metrics.last_round_start);
	}
};

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

#endif
