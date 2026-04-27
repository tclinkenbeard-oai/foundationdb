/*
 * ClusterHealthIFactor.actor.cpp
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

#include <algorithm>
#include <limits>

#include "fdbserver/ClusterHealthIFactor.h"
#include "fdbserver/ClusterHealthMonitor.h"
#include "fdbserver/RecoveryState.h"
#include "flow/Trace.h"

#include "flow/actorcompiler.h" // This must be the last #include.

namespace cluster_health {

namespace {

WorkerEvents filterEmptyEvents(WorkerEvents const& events) {
	WorkerEvents filteredEvents;
	for (auto const& event : events) {
		if (event.second.size() > 0) {
			filteredEvents.emplace(event.first, event.second);
		}
	}
	return filteredEvents;
}

ACTOR Future<Level> fetchSpaceLevel(LatestWorkerEvents eventsAndErrors,
                                    std::string availableBytesField,
                                    std::string totalBytesField,
                                    double interventionThreshold,
                                    double criticalInterventionThreshold,
                                    const char* failureTraceEventName) {
	if (!eventsAndErrors.present()) {
		return Level::METRICS_MISSING;
	}

	state WorkerEvents filteredEvents = filterEmptyEvents(eventsAndErrors.get().first);
	if (filteredEvents.empty()) {
		return Level::METRICS_MISSING;
	}

	state double minAvailableSpaceRatio = 1.0;

	try {
		for (auto const& event : filteredEvents) {
			double available = event.second.getDouble(availableBytesField);
			double total = std::max(1.0, event.second.getDouble(totalBytesField));
			minAvailableSpaceRatio = std::min(minAvailableSpaceRatio, available / total);
		}

		if (minAvailableSpaceRatio < criticalInterventionThreshold) {
			return Level::CRITICAL_INTERVENTION_REQUIRED;
		}
		if (minAvailableSpaceRatio < interventionThreshold) {
			return Level::INTERVENTION_REQUIRED;
		}
		return Level::HEALTHY;
	} catch (Error& e) {
		TraceEvent(SevWarnAlways, failureTraceEventName).error(e);
		return Level::METRICS_MISSING;
	}
}

} // namespace

StorageSpaceFactor::StorageSpaceFactor(double interventionThreshold, double criticalInterventionThreshold)
  : interventionThreshold(interventionThreshold), criticalInterventionThreshold(criticalInterventionThreshold) {}

std::string_view StorageSpaceFactor::getName() const {
	return "StorageSpace";
}

ACTOR Future<Level> storageSpaceFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                           TrackCodeProbes trackCodeProbes,
                                           double interventionThreshold,
                                           double criticalInterventionThreshold) {
	state LatestWorkerEvents eventsAndErrors =
	    wait(workerEventProvider->getLatestStorageServerEvents("StorageMetrics"));
	state Level level = wait(fetchSpaceLevel(eventsAndErrors,
	                                         "KvstoreBytesAvailable",
	                                         "KvstoreBytesTotal",
	                                         interventionThreshold,
	                                         criticalInterventionThreshold,
	                                         "StorageSpaceFactorFetchFailed"));
	CODE_PROBE(trackCodeProbes && level == Level::HEALTHY, "ClusterHealth StorageSpaceFactor returns HEALTHY");
	CODE_PROBE(trackCodeProbes && level == Level::INTERVENTION_REQUIRED,
	           "ClusterHealth StorageSpaceFactor returns INTERVENTION_REQUIRED");
	CODE_PROBE(trackCodeProbes && level == Level::CRITICAL_INTERVENTION_REQUIRED,
	           "ClusterHealth StorageSpaceFactor returns CRITICAL_INTERVENTION_REQUIRED");
	return level;
}

Future<Level> StorageSpaceFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                             TrackCodeProbes trackCodeProbes) {
	return storageSpaceFetchLevel(
	    workerEventProvider, trackCodeProbes, interventionThreshold, criticalInterventionThreshold);
}

TLogSpaceFactor::TLogSpaceFactor(double interventionThreshold, double criticalInterventionThreshold)
  : interventionThreshold(interventionThreshold), criticalInterventionThreshold(criticalInterventionThreshold) {}

std::string_view TLogSpaceFactor::getName() const {
	return "TLogSpace";
}

ACTOR Future<Level> tLogSpaceFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                        TrackCodeProbes trackCodeProbes,
                                        double interventionThreshold,
                                        double criticalInterventionThreshold) {
	state LatestWorkerEvents eventsAndErrors = wait(workerEventProvider->getLatestTLogEvents("TLogMetrics"));
	state Level level = wait(fetchSpaceLevel(eventsAndErrors,
	                                         "QueueDiskBytesAvailable",
	                                         "QueueDiskBytesTotal",
	                                         interventionThreshold,
	                                         criticalInterventionThreshold,
	                                         "TLogSpaceFactorFetchFailed"));
	CODE_PROBE(trackCodeProbes && level == Level::HEALTHY, "ClusterHealth TLogSpaceFactor returns HEALTHY");
	CODE_PROBE(trackCodeProbes && level == Level::INTERVENTION_REQUIRED,
	           "ClusterHealth TLogSpaceFactor returns INTERVENTION_REQUIRED");
	CODE_PROBE(trackCodeProbes && level == Level::CRITICAL_INTERVENTION_REQUIRED,
	           "ClusterHealth TLogSpaceFactor returns CRITICAL_INTERVENTION_REQUIRED");
	return level;
}

Future<Level> TLogSpaceFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                          TrackCodeProbes trackCodeProbes) {
	return tLogSpaceFetchLevel(
	    workerEventProvider, trackCodeProbes, interventionThreshold, criticalInterventionThreshold);
}

std::string_view StorageReplicationFactor::getName() const {
	return "StorageReplication";
}

ACTOR Future<Level> storageReplicationFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                                 TrackCodeProbes trackCodeProbes) {
	state LatestWorkerEvents eventsAndErrors = wait(workerEventProvider->getLatestDataDistributorEvents("MovingData"));
	if (!eventsAndErrors.present()) {
		return Level::METRICS_MISSING;
	}

	state WorkerEvents filteredEvents = filterEmptyEvents(eventsAndErrors.get().first);
	if (filteredEvents.empty()) {
		return Level::METRICS_MISSING;
	}

	try {
		state int64_t queuedOrInFlightRepairMoves = 0;
		state int64_t zeroReplicaTeams = 0;
		state int64_t oneReplicaTeams = 0;
		for (auto const& event : filteredEvents) {
			int64_t inQueue = event.second.getInt64("InQueue");
			int64_t inFlight = event.second.getInt64("InFlight");
			int64_t priorityTeamUnhealthy = event.second.getInt64("PriorityTeamUnhealthy");
			int64_t priorityTeam2Left = event.second.getInt64("PriorityTeam2Left");
			int64_t priorityTeam1Left = event.second.getInt64("PriorityTeam1Left");
			int64_t priorityTeam0Left = event.second.getInt64("PriorityTeam0Left");

			zeroReplicaTeams += priorityTeam0Left;
			oneReplicaTeams += priorityTeam1Left;
			if (inQueue > 0 || inFlight > 0) {
				queuedOrInFlightRepairMoves +=
				    priorityTeamUnhealthy + priorityTeam2Left + priorityTeam1Left + priorityTeam0Left;
			}
		}

		if (zeroReplicaTeams > 0) {
			CODE_PROBE(trackCodeProbes, "ClusterHealth StorageReplicationFactor returns OUTAGE");
			return Level::OUTAGE;
		}
		if (oneReplicaTeams > 0 && workerEventProvider->shouldTreatStorageTeamOneReplicaLeftAsCritical()) {
			CODE_PROBE(trackCodeProbes,
			           "ClusterHealth StorageReplicationFactor returns CRITICAL_INTERVENTION_REQUIRED");
			return Level::CRITICAL_INTERVENTION_REQUIRED;
		}
		if (queuedOrInFlightRepairMoves > 0) {
			CODE_PROBE(trackCodeProbes, "ClusterHealth StorageReplicationFactor returns SELF_HEALING");
			return Level::SELF_HEALING;
		}
		CODE_PROBE(trackCodeProbes, "ClusterHealth StorageReplicationFactor returns HEALTHY");
		return Level::HEALTHY;
	} catch (Error& e) {
		TraceEvent(SevWarnAlways, "StorageReplicationFactorFetchFailed").error(e);
		return Level::METRICS_MISSING;
	}
}

Future<Level> StorageReplicationFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                                   TrackCodeProbes trackCodeProbes) {
	return storageReplicationFetchLevel(workerEventProvider, trackCodeProbes);
}

std::string_view RecoveryStateFactor::getName() const {
	return "RecoveryState";
}

ACTOR Future<Level> recoveryStateFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                            TrackCodeProbes trackCodeProbes) {
	state Optional<RecoveryState> recoveryState = workerEventProvider->getRecoveryState();
	if (!recoveryState.present()) {
		return Level::METRICS_MISSING;
	}

	state Level level = Level::HEALTHY;
	if (recoveryState.get() < RecoveryState::ACCEPTING_COMMITS) {
		level = Level::OUTAGE;
	} else if (recoveryState.get() < RecoveryState::FULLY_RECOVERED) {
		level = Level::SELF_HEALING;
	}

	CODE_PROBE(trackCodeProbes && level == Level::OUTAGE, "ClusterHealth RecoveryStateFactor returns OUTAGE");
	CODE_PROBE(trackCodeProbes && level == Level::SELF_HEALING,
	           "ClusterHealth RecoveryStateFactor returns SELF_HEALING");
	CODE_PROBE(trackCodeProbes && level == Level::HEALTHY, "ClusterHealth RecoveryStateFactor returns HEALTHY");
	return level;
}

Future<Level> RecoveryStateFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                              TrackCodeProbes trackCodeProbes) {
	return recoveryStateFetchLevel(workerEventProvider, trackCodeProbes);
}

std::string_view ProcessErrorsFactor::getName() const {
	return "ProcessErrors";
}

ACTOR Future<Level> processErrorsFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                            TrackCodeProbes trackCodeProbes) {
	state LatestWorkerEvents eventsAndErrors = wait(workerEventProvider->getLatestEvents(""));
	if (!eventsAndErrors.present()) {
		return Level::METRICS_MISSING;
	}

	state WorkerEvents filteredEvents = filterEmptyEvents(eventsAndErrors.get().first);
	if (filteredEvents.empty()) {
		bool const hadSuccessfulRequest = eventsAndErrors.get().first.size() > eventsAndErrors.get().second.size();
		CODE_PROBE(trackCodeProbes && hadSuccessfulRequest, "ClusterHealth ProcessErrorsFactor returns HEALTHY");
		return hadSuccessfulRequest ? Level::HEALTHY : Level::METRICS_MISSING;
	}
	CODE_PROBE(trackCodeProbes, "ClusterHealth ProcessErrorsFactor returns CRITICAL_INTERVENTION_REQUIRED");
	return Level::CRITICAL_INTERVENTION_REQUIRED;
}

Future<Level> ProcessErrorsFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                              TrackCodeProbes trackCodeProbes) {
	return processErrorsFetchLevel(workerEventProvider, trackCodeProbes);
}

RkThrottlingFactor::RkThrottlingFactor(double criticalTpsLimitToReleasedTpsRatioThreshold)
  : criticalTpsLimitToReleasedTpsRatioThreshold(criticalTpsLimitToReleasedTpsRatioThreshold) {}

std::string_view RkThrottlingFactor::getName() const {
	return "RkThrottling";
}

ACTOR Future<Level> rkThrottlingFetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                           TrackCodeProbes trackCodeProbes,
                                           double criticalTpsLimitToReleasedTpsRatioThreshold) {
	state LatestWorkerEvents eventsAndErrors = wait(workerEventProvider->getLatestRatekeeperEvents("RkUpdate"));
	if (!eventsAndErrors.present()) {
		return Level::METRICS_MISSING;
	}

	state WorkerEvents filteredEvents = filterEmptyEvents(eventsAndErrors.get().first);
	if (filteredEvents.empty()) {
		return Level::METRICS_MISSING;
	}

	try {
		state double minTpsLimitToReleasedTpsRatio = std::numeric_limits<double>::infinity();
		for (auto const& event : filteredEvents) {
			double tpsLimit = event.second.getDouble("TPSLimit");
			if (tpsLimit == 0.0) {
				CODE_PROBE(trackCodeProbes, "ClusterHealth RkThrottlingFactor returns OUTAGE");
				return Level::OUTAGE;
			}

			double releasedTps = event.second.getDouble("ReleasedTPS");
			if (releasedTps == 0.0) {
				continue;
			}

			minTpsLimitToReleasedTpsRatio = std::min(minTpsLimitToReleasedTpsRatio, tpsLimit / releasedTps);
		}

		if (minTpsLimitToReleasedTpsRatio < criticalTpsLimitToReleasedTpsRatioThreshold) {
			CODE_PROBE(trackCodeProbes, "ClusterHealth RkThrottlingFactor returns CRITICAL_INTERVENTION_REQUIRED");
			return Level::CRITICAL_INTERVENTION_REQUIRED;
		}
		CODE_PROBE(trackCodeProbes, "ClusterHealth RkThrottlingFactor returns HEALTHY");
		return Level::HEALTHY;
	} catch (Error& e) {
		TraceEvent(SevWarnAlways, "RkThrottlingFactorFetchFailed").error(e);
		return Level::METRICS_MISSING;
	}
}

Future<Level> RkThrottlingFactor::fetchLevel(Reference<IWorkerEventProvider const> workerEventProvider,
                                             TrackCodeProbes trackCodeProbes) {
	return rkThrottlingFetchLevel(workerEventProvider, trackCodeProbes, criticalTpsLimitToReleasedTpsRatioThreshold);
}

} // namespace cluster_health
