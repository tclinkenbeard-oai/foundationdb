/*
 * ClientApiBoundary.cpp
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

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/SpecialKeySpace.h"
#include "fdbserver/core/TesterInterface.h"
#include "fdbserver/tester/workloads.h"
#include "flow/Coroutines.h"
#include "flow/Error.h"
#include "flow/IRandom.h"

struct ClientApiBoundaryWorkload : TestWorkload {
	static constexpr auto NAME = "ClientApiBoundary";

	explicit ClientApiBoundaryWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {}

	Future<Void> setup(Database const& cx) override {
		if (clientId != 0) {
			return Void();
		}
		return seed(cx);
	}

	Future<Void> start(Database const& cx) override {
		if (clientId != 0) {
			return Void();
		}
		return run(cx);
	}

	Future<bool> check(Database const& cx) override { return true; }
	void getMetrics(std::vector<PerfMetric>& m) override {}

	Future<Void> seed(Database cx) {
		Transaction tr(cx);
		while (true) {
			Error err;
			try {
				tr.clear(KeyRangeRef("clientApiBoundary/"_sr, "clientApiBoundary0"_sr));
				tr.set("clientApiBoundary/a"_sr, "a"_sr);
				tr.set("clientApiBoundary/b"_sr, "b"_sr);
				tr.set("clientApiBoundary/c"_sr, "c"_sr);
				tr.set("clientApiBoundary/d"_sr, "d"_sr);
				co_await tr.commit();
				co_return;
			} catch (Error& e) {
				err = e;
			}
			if (err.code() == error_code_actor_cancelled) {
				throw err;
			}
			co_await tr.onError(err);
		}
	}

	static void checkKeys(RangeResult const& result, KeyRef first, KeyRef second) {
		ASSERT_EQ(result.size(), 2);
		ASSERT_EQ(result[0].key, first);
		ASSERT_EQ(result[1].key, second);
	}

	Future<Void> checkRegularRanges(Database cx) {
		Key a = "clientApiBoundary/a"_sr;
		Key b = "clientApiBoundary/b"_sr;
		Key c = "clientApiBoundary/c"_sr;
		while (true) {
			Transaction native(cx);
			ReadYourWritesTransaction ryw(cx);
			ReadYourWritesTransaction disabled(cx);
			Error err;
			try {
				native.setOption(FDBTransactionOptions::TAG, "client-api-boundary"_sr);
				native.setOption(FDBTransactionOptions::AUTO_THROTTLE_TAG, "client-api-boundary"_sr);
				disabled.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);

				KeySelector begin(firstGreaterThan(a), a.arena());
				KeySelector end(firstGreaterThan(c), c.arena());
				RangeResult nativeForward =
				    co_await native.getRange(begin, end, GetRangeLimits(10), Snapshot::False, Reverse::False);
				checkKeys(nativeForward, b, c);
				RangeResult nativeReverse =
				    co_await native.getRange(begin, end, GetRangeLimits(10), Snapshot::False, Reverse::True);
				checkKeys(nativeReverse, c, b);
				RangeResult rywForward =
				    co_await ryw.getRange(begin, end, GetRangeLimits(10), Snapshot::False, Reverse::False);
				checkKeys(rywForward, b, c);
				RangeResult rywReverse =
				    co_await ryw.getRange(begin, end, GetRangeLimits(10), Snapshot::False, Reverse::True);
				checkKeys(rywReverse, c, b);
				RangeResult nativeSnapshot =
				    co_await native.getRange(begin, end, GetRangeLimits(10), Snapshot::True, Reverse::False);
				checkKeys(nativeSnapshot, b, c);
				RangeResult rywSnapshot =
				    co_await ryw.getRange(begin, end, GetRangeLimits(10), Snapshot::True, Reverse::False);
				checkKeys(rywSnapshot, b, c);
				RangeResult disabledForward =
				    co_await disabled.getRange(begin, end, GetRangeLimits(10), Snapshot::False, Reverse::False);
				checkKeys(disabledForward, b, c);
				RangeResult disabledSnapshotReverse =
				    co_await disabled.getRange(begin, end, GetRangeLimits(10), Snapshot::True, Reverse::True);
				checkKeys(disabledSnapshotReverse, c, b);

				KeySelector invertedBegin(firstGreaterThan(c), c.arena());
				KeySelector invertedEnd(firstGreaterOrEqual(b), b.arena());
				ASSERT((co_await native.getRange(
				            invertedBegin, invertedEnd, GetRangeLimits(10), Snapshot::False, Reverse::False))
				           .empty());
				ASSERT((co_await ryw.getRange(
				            invertedBegin, invertedEnd, GetRangeLimits(10), Snapshot::False, Reverse::False))
				           .empty());

				GetRangeLimits zeroRows(0, 100);
				GetRangeLimits zeroBytes(GetRangeLimits::ROW_LIMIT_UNLIMITED, 0);
				zeroBytes.minRows = 0;
				ASSERT((co_await native.getRange(begin, end, zeroRows, Snapshot::False, Reverse::False)).empty());
				ASSERT((co_await native.getRange(begin, end, zeroBytes, Snapshot::False, Reverse::True)).empty());
				ASSERT((co_await ryw.getRange(begin, end, zeroRows, Snapshot::False, Reverse::False)).empty());
				ASSERT((co_await ryw.getRange(begin, end, zeroBytes, Snapshot::False, Reverse::True)).empty());

				GetRangeLimits invalid(-2, 100);
				Error nativeInvalid;
				try {
					co_await native.getRange(begin, end, invalid, Snapshot::False, Reverse::False);
				} catch (Error& e) {
					nativeInvalid = e;
				}
				ASSERT_EQ(nativeInvalid.code(), error_code_range_limits_invalid);
				Error rywInvalid;
				try {
					co_await ryw.getRange(begin, end, invalid, Snapshot::False, Reverse::False);
				} catch (Error& e) {
					rywInvalid = e;
				}
				ASSERT_EQ(rywInvalid.code(), error_code_range_limits_invalid);
				co_return;
			} catch (Error& e) {
				err = e;
			}
			if (err.code() == error_code_actor_cancelled) {
				throw err;
			}
			co_await native.onError(err);
		}
	}

	Future<Void> checkStreamRanges(Database cx) {
		Key a = "clientApiBoundary/a"_sr;
		Key b = "clientApiBoundary/b"_sr;
		Key c = "clientApiBoundary/c"_sr;
		while (true) {
			Transaction tr(cx);
			Error err;
			try {
				PromiseStream<RangeResult> results;
				Future<Void> stream = tr.getRangeStream(results,
				                                        KeySelector(firstGreaterThan(a), a.arena()),
				                                        KeySelector(firstGreaterThan(c), c.arena()),
				                                        GetRangeLimits(),
				                                        Snapshot::False,
				                                        Reverse::False);
				std::vector<Key> keys;
				Error streamError;
				try {
					while (true) {
						RangeResult part = co_await results.getFuture();
						for (auto const& kv : part) {
							keys.emplace_back(kv.key);
						}
					}
				} catch (Error& e) {
					streamError = e;
				}
				if (streamError.code() != error_code_end_of_stream) {
					stream.cancel();
					throw streamError;
				}
				co_await stream;
				ASSERT_EQ(keys.size(), 2);
				ASSERT_EQ(keys[0], b);
				ASSERT_EQ(keys[1], c);

				PromiseStream<RangeResult> invertedResults;
				Future<Void> inverted = tr.getRangeStream(invertedResults,
				                                          KeySelector(firstGreaterThan(c), c.arena()),
				                                          KeySelector(firstGreaterOrEqual(b), b.arena()),
				                                          GetRangeLimits(),
				                                          Snapshot::False,
				                                          Reverse::False);
				Error invertedError;
				try {
					co_await invertedResults.getFuture();
				} catch (Error& e) {
					invertedError = e;
				}
				ASSERT_EQ(invertedError.code(), error_code_end_of_stream);
				co_await inverted;
				co_return;
			} catch (Error& e) {
				err = e;
			}
			if (err.code() == error_code_actor_cancelled) {
				throw err;
			}
			co_await tr.onError(err);
		}
	}

	Future<Void> checkUnsupportedMappedRanges(Database cx) {
		Key begin = "clientApiBoundary/a"_sr;
		Key end = "clientApiBoundary/d"_sr;
		Key mapper = "client-api-boundary-mapper"_sr;
		KeySelector beginSelector(firstGreaterOrEqual(begin), begin.arena());
		KeySelector endSelector(firstGreaterOrEqual(end), end.arena());

		Error nativeError;
		try {
			Transaction native(cx);
			co_await native.getMappedRange(
			    beginSelector, endSelector, mapper, GetRangeLimits(10), Snapshot::False, Reverse::False);
		} catch (Error& e) {
			nativeError = e;
		}
		ASSERT_EQ(nativeError.code(), error_code_unsupported_operation);

		Error snapshotError;
		try {
			ReadYourWritesTransaction ryw(cx);
			co_await ryw.getMappedRange(
			    beginSelector, endSelector, mapper, GetRangeLimits(10), Snapshot::True, Reverse::False);
		} catch (Error& e) {
			snapshotError = e;
		}
		ASSERT_EQ(snapshotError.code(), error_code_unsupported_operation);

		Error rywDisabledError;
		try {
			ReadYourWritesTransaction ryw(cx);
			ryw.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);
			co_await ryw.getMappedRange(
			    beginSelector, endSelector, mapper, GetRangeLimits(10), Snapshot::False, Reverse::False);
		} catch (Error& e) {
			rywDisabledError = e;
		}
		ASSERT_EQ(rywDisabledError.code(), error_code_unsupported_operation);

		Error specialKeyError;
		try {
			ReadYourWritesTransaction ryw(cx);
			co_await ryw.getMappedRange(KeySelector(firstGreaterOrEqual(specialKeys.begin)),
			                            KeySelector(firstGreaterOrEqual(specialKeys.end)),
			                            mapper,
			                            GetRangeLimits(10),
			                            Snapshot::False,
			                            Reverse::False);
		} catch (Error& e) {
			specialKeyError = e;
		}
		ASSERT_EQ(specialKeyError.code(), error_code_client_invalid_operation);
	}

	Future<Void> checkOptionsAndWatch(Database cx) {
		Error debugIdentifierError;
		try {
			Transaction native(cx);
			native.setOption(FDBTransactionOptions::DEBUG_TRANSACTION_IDENTIFIER, ""_sr);
		} catch (Error& e) {
			debugIdentifierError = e;
		}
		ASSERT_EQ(debugIdentifierError.code(), error_code_invalid_option_value);

		Error disableAfterWriteError;
		try {
			ReadYourWritesTransaction ryw(cx);
			ryw.set("clientApiBoundary/option"_sr, "written"_sr);
			ryw.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);
		} catch (Error& e) {
			disableAfterWriteError = e;
		}
		ASSERT_EQ(disableAfterWriteError.code(), error_code_client_invalid_operation);

		ReadYourWritesTransaction watchesDisabled(cx);
		watchesDisabled.setOption(FDBTransactionOptions::READ_YOUR_WRITES_DISABLE);
		Error watchError;
		try {
			co_await watchesDisabled.watch("clientApiBoundary/watch"_sr);
		} catch (Error& e) {
			watchError = e;
		}
		ASSERT_EQ(watchError.code(), error_code_watches_disabled);

		int64_t noDelay = 0;
		Transaction retry(cx);
		retry.setOption(FDBTransactionOptions::MAX_RETRY_DELAY,
		                StringRef(reinterpret_cast<const uint8_t*>(&noDelay), sizeof(noDelay)));
		co_await retry.onError(not_committed());
	}

	Future<Void> checkSpecialKeyErrors(Database cx) {
		const KeyRangeRef tracing = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::TRACING);
		const Key token = tracing.begin.withSuffix("token"_sr);
		const Key transactionId = tracing.begin.withSuffix("transaction_id"_sr);

		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			const UID id(1, 2);
			tr.set(transactionId, Value(id.toString()));
			tr.set(token, "true"_sr);
			RangeResult result = co_await tr.getRange(tracing, CLIENT_KNOBS->TOO_MANY);
			ASSERT_EQ(result.size(), 2);
			ASSERT_EQ(result[0].key, token);
			ASSERT(!result[0].value.empty());
			ASSERT_EQ(result[1].key, transactionId);
			ASSERT_EQ(result[1].value, Value(id.toString()));
		}
		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr.set(token, "false"_sr);
			Optional<Value> value = co_await tr.get(token);
			ASSERT(value.present());
			ASSERT_EQ(value.get(), "0"_sr);
		}
		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			Error err;
			try {
				tr.set(token, "invalid"_sr);
			} catch (Error& e) {
				err = e;
			}
			ASSERT_EQ(err.code(), error_code_special_keys_api_failure);
			ASSERT(tr.getSpecialKeySpaceErrorMsg().present());
			ASSERT(tr.getSpecialKeySpaceErrorMsg().get().find("token must be set to true/false") != std::string::npos);
		}
		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			Error err;
			try {
				tr.clear(token);
			} catch (Error& e) {
				err = e;
			}
			ASSERT_EQ(err.code(), error_code_special_keys_api_failure);
			ASSERT(tr.getSpecialKeySpaceErrorMsg().present());
		}
		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			Error err;
			try {
				tr.clear(tracing);
			} catch (Error& e) {
				err = e;
			}
			ASSERT_EQ(err.code(), error_code_special_keys_api_failure);
			ASSERT(tr.getSpecialKeySpaceErrorMsg().present());
		}
		{
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			tr.set("clientApiBoundary/tracing-late"_sr, "written"_sr);
			tr.set(token, "true"_sr);
			Error err;
			try {
				co_await tr.commit();
			} catch (Error& e) {
				err = e;
			}
			ASSERT_EQ(err.code(), error_code_special_keys_api_failure);
			ASSERT(tr.getSpecialKeySpaceErrorMsg().present());
			ASSERT(tr.getSpecialKeySpaceErrorMsg().get().find("tracing options must be set first") !=
			       std::string::npos);
		}
		{
			const KeyRangeRef global = SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::GLOBALCONFIG);
			const Key clearKey = global.begin.withSuffix("client_api_boundary/unused"_sr);
			const Key clearEnd = global.begin.withSuffix("client_api_boundary/unused0"_sr);
			ReadYourWritesTransaction tr(cx);
			tr.setOption(FDBTransactionOptions::SPECIAL_KEY_SPACE_ENABLE_WRITES);
			co_await tr.getRange(global, CLIENT_KNOBS->TOO_MANY);
			tr.clear(clearKey);
			tr.clear(KeyRangeRef(clearKey, clearEnd));
		}
	}

	Future<Void> run(Database cx) {
		co_await checkRegularRanges(cx);
		co_await checkStreamRanges(cx);
		co_await checkUnsupportedMappedRanges(cx);
		co_await checkOptionsAndWatch(cx);
		co_await checkSpecialKeyErrors(cx);
	}
};

WorkloadFactory<ClientApiBoundaryWorkload> ClientApiBoundaryWorkloadFactory;
