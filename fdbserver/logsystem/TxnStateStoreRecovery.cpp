/*
 * TxnStateStoreRecovery.cpp
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

#include "fdbserver/logsystem/TxnStateStoreRecovery.h"

#include "fdbclient/SystemData.h"
#include "fdbserver/core/Knobs.h"
#include "fdbserver/core/ServerDBInfo.h"
#include "fdbserver/logsystem/ApplyMetadataMutation.h"

TxnStateRequestAccumulator::TxnStateRequestAccumulator(IKeyValueStore* txnStateStore,
                                                       PromiseStream<Future<Void>>* actors)
  : txnStateStore(txnStateStore), actors(actors) {
	ASSERT(txnStateStore != nullptr);
	ASSERT(actors != nullptr);
}

Future<Void> TxnStateRequestAccumulator::processRequestPart(TxnStateRequest request,
                                                            CompleteCallback processComplete,
                                                            BeforeStoreCallback beforeStore,
                                                            bool waitForCompletionOnDuplicate) {
	ASSERT(txnStateStore != nullptr);
	ASSERT(actors != nullptr);
	ASSERT(processComplete);

	if (receivedSequences.contains(request.sequence)) {
		if (waitForCompletionOnDuplicate && receivedSequences.size() == maxSequence) {
			co_await replay;
		}

		// This part is already received. Still rebroadcast it to peers.
		actors->send(broadcastTxnRequest(request, SERVER_KNOBS->TXN_STATE_SEND_AMOUNT, true));
		co_await yield();
		co_return;
	}

	if (request.last) {
		// This is the last piece of the sequence, but other pieces may still be in flight.
		maxSequence = request.sequence + 1;
	}
	receivedSequences.insert(request.sequence);

	if (beforeStore) {
		beforeStore();
	}

	for (auto& kv : request.data) {
		txnStateStore->set(kv, &request.arena);
	}
	txnStateStore->commit(true);

	if (receivedSequences.size() == maxSequence) {
		// Received all components of the txnStateRequest.
		ASSERT(!processed);
		replay = processComplete();
		co_await replay;
		processed = true;
	}

	actors->send(broadcastTxnRequest(request, SERVER_KNOBS->TXN_STATE_SEND_AMOUNT, true));
	co_await yield();
}

TxnStateStoreReplayContext::TxnStateStoreReplayContext(IKeyValueStore* txnStateStore,
                                                       std::map<UID, Reference<StorageInfo>>* storageCache,
                                                       KeyRangeMap<ServerCacheInfo>* keyInfo)
  : txnStateStore(txnStateStore), storageCache(storageCache), keyInfo(keyInfo) {
	ASSERT(txnStateStore != nullptr);
	ASSERT(storageCache != nullptr);
	ASSERT(keyInfo != nullptr);
}

bool TxnStateStoreReplayContext::consumeNonKeyServersKeyValue(const KeyValueRef&) {
	return false;
}

Future<Void> replayTxnStateStore(TxnStateStoreReplayContext& context) {
	KeyRange txnKeys = allKeys;
	std::map<Tag, UID> tagUid;

	RangeResult uidToTagMap = context.txnStateStore->readRange(serverTagKeys).get();
	for (const KeyValueRef& kv : uidToTagMap) {
		tagUid[decodeServerTagValue(kv.value)] = decodeServerTagKey(kv.key);
	}

	while (true) {
		co_await yield();

		RangeResult data =
		    context.txnStateStore
		        ->readRange(txnKeys, SERVER_KNOBS->BUGGIFIED_ROW_LIMIT, SERVER_KNOBS->APPLY_MUTATION_BYTES)
		        .get();
		if (data.empty()) {
			break;
		}

		((KeyRangeRef&)txnKeys) = KeyRangeRef(keyAfter(data.back().key, txnKeys.arena()), txnKeys.end);

		Standalone<VectorRef<MutationRef>> mutations;
		std::vector<std::pair<MapPair<Key, ServerCacheInfo>, int>> keyInfoData;
		std::vector<UID> src;
		std::vector<UID> dest;
		ServerCacheInfo info;
		auto updateTagInfo = [&context](const std::vector<UID>& uids,
		                                std::vector<Tag>& tags,
		                                std::vector<Reference<StorageInfo>>& storageInfoItems) {
			for (const auto& id : uids) {
				auto storageInfo = getStorageInfo(id, context.storageCache, context.txnStateStore);
				ASSERT(storageInfo->tag != invalidTag);
				tags.push_back(storageInfo->tag);
				storageInfoItems.push_back(storageInfo);
			}
		};

		for (auto& kv : data) {
			if (kv.key.startsWith(keyServersPrefix)) {
				KeyRef key = kv.key.removePrefix(keyServersPrefix);
				if (key == allKeys.end) {
					continue;
				}
				decodeKeyServersValue(tagUid, kv.value, src, dest);

				info.tags.clear();

				info.src_info.clear();
				updateTagInfo(src, info.tags, info.src_info);

				info.dest_info.clear();
				updateTagInfo(dest, info.tags, info.dest_info);

				uniquify(info.tags);
				keyInfoData.emplace_back(MapPair<Key, ServerCacheInfo>(key, info), 1);
			} else if (!context.consumeNonKeyServersKeyValue(kv)) {
				mutations.emplace_back(mutations.arena(), MutationRef::SetValue, kv.key, kv.value);
			}
		}

		// Insert keyTag data separately from metadata mutations so that one bulk insert
		// avoids a large number of map lookups.
		context.keyInfo->rawInsert(keyInfoData);
		context.applyMetadataBatch(mutations);
	}

	context.finishReplay();
	context.txnStateStore->enableSnapshot();
}
