/*
 * DatabaseContext.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
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

#include "fdbclient/DatabaseContext.h"

Reference<WatchMetadata> DatabaseContext::getWatchMetadata(int64_t tenantId, KeyRef key) const {
	const auto it = watchMap.find(std::make_pair(tenantId, key));
	if (it == watchMap.end())
		return Reference<WatchMetadata>();
	return it->second;
}

void DatabaseContext::setWatchMetadata(Reference<WatchMetadata> metadata) {
	const WatchMapKey key(metadata->parameters->tenant.tenantId, metadata->parameters->key);
	watchMap[key] = metadata;
	// NOTE Here we do *NOT* update/reset the reference count for the key, see the source code in getWatchFuture.
	// Basically the reference count could be increased, or the same watch is refreshed, or the watch might be cancelled
}

int32_t DatabaseContext::increaseWatchRefCount(const int64_t tenantID, KeyRef key, const Version& version) {
	const WatchMapKey mapKey(tenantID, key);
	watchCounterMap[mapKey].insert(version);
	return watchCounterMap[mapKey].size();
}

int32_t DatabaseContext::decreaseWatchRefCount(const int64_t tenantID, KeyRef key, const Version& version) {
	const WatchMapKey mapKey(tenantID, key);
	auto mapKeyIter = watchCounterMap.find(mapKey);
	if (mapKeyIter == std::end(watchCounterMap)) {
		// Key does not exist. The metadata might be removed by deleteWatchMetadata already.
		return 0;
	}

	auto& versionSet = mapKeyIter->second;
	auto versionIter = versionSet.find(version);

	if (versionIter == std::end(versionSet)) {
		// Version not found, the watch might be cleared before.
		return versionSet.size();
	}
	versionSet.erase(versionIter);

	const auto count = versionSet.size();
	// The metadata might be deleted somewhere else, before calling this decreaseWatchRefCount
	if (auto metadata = getWatchMetadata(tenantID, key); metadata.isValid() && versionSet.size() == 0) {
		// It is a *must* to cancel the watchFutureSS manually. watchFutureSS waits for watchStorageServerResp, which
		// holds a reference to the metadata. If the ACTOR is not cancelled, it indirectly holds a Future waiting for
		// itself.
		metadata->watchFutureSS.cancel();
		deleteWatchMetadata(tenantID, key);
	}

	return count;
}

void DatabaseContext::deleteWatchMetadata(int64_t tenantId, KeyRef key, bool removeReferenceCount) {
	const WatchMapKey mapKey(tenantId, key);
	watchMap.erase(mapKey);
	if (removeReferenceCount) {
		watchCounterMap.erase(mapKey);
	}
}

void DatabaseContext::shutdown() {
	if (shutdownStarted.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
	cacheListMonitor.cancel();
	clientInfoMonitor.cancel();
	clientDBInfoMonitor.cancel();
	monitorTssInfoChange.cancel();
	tssMismatchHandler.cancel();
	if (clientStatusUpdater.actor.isValid()) {
		clientStatusUpdater.actor.cancel();
	}
	if (globalConfig) {
		globalConfig.reset();
	}
	if (clientInfo) {
		clientInfo->set(ClientDBInfo());
		clientInfo.clear();
	}
	if (grvUpdateHandler.isValid()) {
		grvUpdateHandler.cancel();
	}
	for (auto& [_, batcher] : versionBatcher) {
		if (batcher.actor.isValid()) {
			batcher.actor.cancel();
		}
	}
	versionBatcher.clear();
	std::vector<Reference<WatchMetadata>> watchesToCancel;
	watchesToCancel.reserve(watchMap.size());
	for (auto& [_, metadata] : watchMap) {
		watchesToCancel.push_back(metadata);
	}
	watchMap.clear();
	watchCounterMap.clear();
	for (auto& metadata : watchesToCancel) {
		if (metadata->watchFutureSS.isValid()) {
			metadata->watchFutureSS.cancel();
		}
		if (metadata->watchPromise.canBeSet()) {
			metadata->watchPromise.sendError(actor_cancelled());
		}
	}
	if (logger.isValid()) {
		logger.cancel();
	}
	if (throttleExpirer.isValid()) {
		throttleExpirer.cancel();
	}
	if (statusLeaderMon.isValid()) {
		statusLeaderMon.cancel();
	}
	if (changeFeedStorageCommitter.isValid()) {
		changeFeedStorageCommitter.cancel();
	}
	if (initializeChangeFeedCache.isValid()) {
		initializeChangeFeedCache.cancel();
	}
	storage = nullptr;
	if (sharedStatePtr) {
		sharedStatePtr->delRef(sharedStatePtr);
		sharedStatePtr = nullptr;
	}
	for (auto it = server_interf.begin(); it != server_interf.end(); it = server_interf.erase(it)) {
		it->second->notifyContextDestroyed();
	}
	ASSERT_ABORT(server_interf.empty());
	locationCache.insert(allKeys, Reference<LocationInfo>());
	for (auto& it : notAtLatestChangeFeeds) {
		it.second->context = nullptr;
	}
	for (auto& it : changeFeedUpdaters) {
		it.second->context = nullptr;
	}
	locationCache.clear();
}
