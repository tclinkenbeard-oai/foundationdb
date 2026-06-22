/*
 * TxnStateStoreRecovery.h
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

#ifndef FDBSERVER_TXN_STATE_STORE_RECOVERY_H
#define FDBSERVER_TXN_STATE_STORE_RECOVERY_H
#pragma once

#include <functional>
#include <limits>
#include <map>
#include <unordered_set>

#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbserver/kvstore/IKeyValueStore.h"
#include "flow/flow.h"

// Receives the partitioned transaction-state broadcast, persists each part, and
// invokes the role-specific replay callback once every part is present.
class TxnStateRequestAccumulator {
public:
	using CompleteCallback = std::function<Future<Void>()>;
	using BeforeStoreCallback = std::function<void()>;

	TxnStateRequestAccumulator() = default;
	TxnStateRequestAccumulator(IKeyValueStore* txnStateStore, PromiseStream<Future<Void>>* actors);

	Future<Void> processRequestPart(TxnStateRequest request,
	                                CompleteCallback processComplete,
	                                BeforeStoreCallback beforeStore = BeforeStoreCallback(),
	                                bool waitForCompletionOnDuplicate = false);

private:
	// Maximum sequence for txnStateRequest, set when the request with last=true is received.
	Sequence maxSequence = std::numeric_limits<Sequence>::max();

	// Requests are processed only after every sequence in [0, maxSequence) has arrived.
	std::unordered_set<Sequence> receivedSequences;

	IKeyValueStore* txnStateStore = nullptr;
	PromiseStream<Future<Void>>* actors = nullptr;

	// Some callers must wait for replay before acknowledging duplicate final parts.
	Future<Void> replay = Void();
	bool processed = false;
};

// Supplies the role-specific behavior needed while rebuilding in-memory state
// from the transaction-state store. The common walker owns keyServers decoding,
// storage-cache population, metadata batching, and snapshot enablement.
class TxnStateStoreReplayContext {
public:
	TxnStateStoreReplayContext(IKeyValueStore* txnStateStore,
	                           std::map<UID, Reference<StorageInfo>>* storageCache,
	                           KeyRangeMap<ServerCacheInfo>* keyInfo);
	virtual ~TxnStateStoreReplayContext() = default;

	// Return true when the role consumed a non-keyServers entry and it should not
	// be included in the metadata mutation batch.
	virtual bool consumeNonKeyServersKeyValue(const KeyValueRef& kv);

	virtual void applyMetadataBatch(const VectorRef<MutationRef>& mutations) = 0;
	virtual void finishReplay() {}

protected:
	IKeyValueStore* getTxnStateStore() const { return txnStateStore; }

private:
	IKeyValueStore* txnStateStore;
	std::map<UID, Reference<StorageInfo>>* storageCache;
	KeyRangeMap<ServerCacheInfo>* keyInfo;

	friend Future<Void> replayTxnStateStore(TxnStateStoreReplayContext& context);
};

Future<Void> replayTxnStateStore(TxnStateStoreReplayContext& context);

#endif
