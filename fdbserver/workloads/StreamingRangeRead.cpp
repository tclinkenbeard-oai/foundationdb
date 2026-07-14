/*
 * StreamingRangeRead.cpp
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

#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/core/FDBSimulationPolicy.h"
#include "fdbserver/core/TesterInterface.h"
#include "fdbserver/tester/workloads.h"
#include "BulkSetup.h"
#include "flow/Arena.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/Trace.h"
#include "flow/serialize.h"

Future<Void> streamUsingGetRange(PromiseStream<RangeResult> results, Transaction* tr, KeyRange keys) {
	KeySelectorRef begin = firstGreaterOrEqual(keys.begin);
	KeySelectorRef end = firstGreaterOrEqual(keys.end);

	try {
		while (true) {
			GetRangeLimits limits(GetRangeLimits::ROW_LIMIT_UNLIMITED, 1e6);
			limits.minRows = 0;
			RangeResult rep = co_await tr->getRange(begin, end, limits, Snapshot::True);

			results.send(rep);

			if (!rep.more) {
				results.sendError(end_of_stream());
				co_return;
			}

			begin = rep.nextBeginKeySelector();
		}
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw;
		}
		results.sendError(e);
		throw;
	}
}

Future<Void> convertStream(PromiseStream<RangeResult> input, PromiseStream<KeyValue> output) {
	try {
		while (true) {
			RangeResult res = co_await input.getFuture();
			for (auto& kv : res) {
				output.send(kv);
			}
		}
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw;
		}
		output.sendError(e);
	}
}

struct StreamingRangeReadWorkload : KVWorkload {
	static constexpr auto NAME = "StreamingRangeRead";
	double testDuration;
	double writeInterval;
	double targetedSSRestartDelay;
	double ssDelayDelay;
	bool performWrites;
	bool requireTssMismatch;
	Future<Void> client;
	Future<Void> writer;
	Future<Void> selectorClient;

	explicit StreamingRangeReadWorkload(WorkloadContext const& wcx) : KVWorkload(wcx) {
		testDuration = getOption(options, "testDuration"_sr, 60.0);
		performWrites = getOption(options, "performWrites"_sr, false);
		writeInterval = getOption(options, "writeInterval"_sr, 0.01);
		targetedSSRestartDelay = getOption(options, "targetedSSRestartDelay"_sr, -1.0);
		ssDelayDelay = getOption(options, "ssDelayDelay"_sr, -1.0);
		requireTssMismatch = getOption(options, "requireTssMismatch"_sr, false);
		ASSERT(nodeCount > 20);
		ASSERT(writeInterval > 0.0);
	}

	Standalone<KeyValueRef> operator()(uint64_t n) { return KeyValueRef(keyForIndex(n, false), randomValue()); }

	Future<Void> setup(Database const& cx) override {
		if (requireTssMismatch) {
			co_await timeoutError(waitForTssMapping(cx->clone()), 60.0);
		}
		co_await bulkSetup(cx, this, nodeCount, Promise<double>());
	}
	Future<Void> start(Database const& cx) override {
		if (targetedSSRestartDelay >= 0.0) {
			fdbSimulationPolicyState().injectTargetedSSRestartTime = now() + targetedSSRestartDelay;
		}
		if (ssDelayDelay >= 0.0) {
			fdbSimulationPolicyState().injectSSDelayTime = now() + ssDelayDelay;
		}
		client = timeout(streamingClient(cx->clone()), testDuration, Void());
		if (performWrites) {
			selectorClient = timeout(checkSelectorsDuringMovement(cx->clone()), testDuration, Void());
			writer = timeout(writingClient(cx->clone()), testDuration, Void());
		}
		return delay(testDuration);
	}

	Future<bool> check(Database const& cx) override {
		client = Void();
		writer = Void();
		selectorClient = Void();
		co_await checkSelectorBoundaries(cx->clone());
		if (requireTssMismatch) {
			co_await timeoutError(checkTssMismatch(cx->clone()), 30.0);
		}
		co_return true;
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}

	Future<Void> waitForTssMapping(Database cx) {
		loop {
			Transaction tr(cx);
			Error err;
			try {
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				RangeResult mappings = co_await tr.getRange(tssMappingKeys, 1);
				if (!mappings.empty()) {
					co_return;
				}
				co_await delay(1.0);
				continue;
			} catch (Error& e) {
				err = e;
			}
			co_await tr.onError(err);
		}
	}

	Future<Void> checkTssMismatch(Database cx) {
		loop {
			Transaction tr(cx);
			Error err;
			try {
				tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
				RangeResult mismatches = co_await tr.getRange(tssMismatchKeys, 1);
				RangeResult quarantined = co_await tr.getRange(tssQuarantineKeys, 1);
				if (!quarantined.empty() && !mismatches.empty()) {
					co_return;
				}
				co_await delay(1.0);
				continue;
			} catch (Error& e) {
				err = e;
			}
			co_await tr.onError(err);
		}
	}

	Future<Void> writingClient(Database cx) {
		while (true) {
			Transaction tr(cx);
			while (true) {
				Error err;
				try {
					tr.set(keyForIndex(deterministicRandom()->randomInt64(0, nodeCount), false), randomValue());
					co_await tr.commit();
					break;
				} catch (Error& e) {
					err = e;
				}
				co_await tr.onError(err);
			}
			co_await delay(writeInterval);
		}
	}

	Future<Void> checkSelectorRange(Database cx,
	                                KeySelector begin,
	                                KeySelector end,
	                                uint64_t expectedBegin,
	                                uint64_t expectedEnd) {
		Transaction tr(cx);
		while (true) {
			PromiseStream<RangeResult> streamResults;
			Future<Void> stream;
			Error err;
			try {
				stream = tr.getRangeStream(streamResults, begin, end, GetRangeLimits(), Snapshot::True);
				RangeResult expected = co_await tr.getRange(begin, end, 100, Snapshot::True);
				ASSERT_EQ(expected.size(), expectedEnd - expectedBegin);
				for (uint64_t i = expectedBegin; i < expectedEnd; ++i) {
					ASSERT(expected[i - expectedBegin].key == keyForIndex(i, false));
				}
				uint64_t index = expectedBegin;
				while (true) {
					Optional<RangeResult> batch;
					try {
						batch = co_await streamResults.getFuture();
					} catch (Error& e) {
						if (e.code() != error_code_end_of_stream) {
							throw;
						}
						break;
					}
					for (auto const& kv : batch.get()) {
						ASSERT(index < expectedEnd);
						ASSERT(kv == expected[index - expectedBegin]);
						++index;
					}
				}
				co_await stream;
				ASSERT_EQ(index, expectedEnd);
				co_return;
			} catch (Error& e) {
				err = e;
			}
			stream.cancel();
			co_await tr.onError(err);
		}
	}

	Future<bool> checkSelectorBoundaries(Database cx) {
		Key begin = keyForIndex(10, false);
		Key end = keyForIndex(20, false);
		co_await checkSelectorRange(cx,
		                            KeySelector(firstGreaterThan(begin), begin.arena()),
		                            KeySelector(firstGreaterThan(end), end.arena()),
		                            11,
		                            21);
		co_await checkSelectorRange(cx,
		                            KeySelector(firstGreaterOrEqual(begin) + 2, begin.arena()),
		                            KeySelector(firstGreaterOrEqual(end) - 2, end.arena()),
		                            12,
		                            18);
		co_await checkSelectorRange(cx,
		                            KeySelector(firstGreaterThan(end), end.arena()),
		                            KeySelector(firstGreaterOrEqual(begin), begin.arena()),
		                            0,
		                            0);
		co_return true;
	}

	Future<Void> checkSelectorsDuringMovement(Database cx) {
		loop {
			co_await checkSelectorBoundaries(cx);
			co_await delay(0.1);
		}
	}

	// Reads the database using both the normal get range API and the streaming API and compares the results
	Future<Void> streamingClient(Database cx) {
		Transaction tr(cx);
		Key next;
		Future<Void> rateLimit = delay(0.01);
		while (true) {
			PromiseStream<RangeResult> streamRaw;
			PromiseStream<RangeResult> compareRaw;
			PromiseStream<KeyValue> streamResults;
			PromiseStream<KeyValue> compareResults;
			Future<Void> compareConvert;
			Future<Void> streamConvert;
			Future<Void> compare;
			Future<Void> stream;

			Error err;
			bool failed = false;
			try {
				compareConvert = convertStream(compareRaw, compareResults);
				streamConvert = convertStream(streamRaw, streamResults);
				stream = tr.getRangeStream(streamRaw,
				                           KeySelector(firstGreaterOrEqual(next), next.arena()),
				                           KeySelector(firstGreaterOrEqual(normalKeys.end)),
				                           GetRangeLimits());
				compare = streamUsingGetRange(compareRaw, &tr, KeyRangeRef(next, normalKeys.end));
				while (true) {
					Optional<KeyValue> cmp;
					Optional<KeyValue> res;
					try {
						KeyValue _cmp = co_await streamResults.getFuture();
						cmp = _cmp;
					} catch (Error& e) {
						if (e.code() != error_code_end_of_stream) {
							throw;
						}
						cmp = Optional<KeyValue>();
					}
					try {
						KeyValue _res = co_await compareResults.getFuture();
						res = _res;
					} catch (Error& e) {
						if (e.code() != error_code_end_of_stream) {
							throw;
						}
						res = Optional<KeyValue>();
					}
					if (cmp != res) {
						TraceEvent(SevError, "RangeStreamMismatch");
						ASSERT(false);
					}
					if (cmp.present()) {
						next = keyAfter(cmp.get().key);
					} else {
						next = Key();
						break;
					}
				}
			} catch (Error& e) {
				err = e;
				failed = true;
			}
			if (failed) {
				co_await tr.onError(err);
			}
			co_await rateLimit;
			rateLimit = delay(0.01);
		}
	}
};

WorkloadFactory<StreamingRangeReadWorkload> StreamingRangeReadWorkloadFactory;
