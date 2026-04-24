/*
 * BenchActorCollection.cpp
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

#include "benchmark/benchmark.h"

#include "flow/ActorCollection.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/flow.h"

#include <cstdint>
#include <vector>

namespace {

struct CleanupCounter {
	uint64_t* cleanupCount;
	~CleanupCounter() { ++*cleanupCount; }
};

Future<Void> cancellableFuture(Future<Void> signal, uint64_t* cleanupCount, uint64_t* cancelledCount) {
	CleanupCounter cleanup{ cleanupCount };
	try {
		co_await signal;
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_actor_cancelled);
		++*cancelledCount;
		throw;
	}
}

Future<Void> benchActorCollectionConstructCancel(benchmark::State* state) {
	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		Future<Void> collection = actorCollection(addActor.getFuture());
		benchmark::DoNotOptimize(collection);
		collection.cancel();
		benchmark::DoNotOptimize(collection);
		benchmark::ClobberMemory();
	}
	state->SetItemsProcessed(state->iterations());
	co_return;
}

Future<Void> benchActorCollectionReadyChild(benchmark::State* state) {
	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		Future<Void> collection = actorCollection(addActor.getFuture(), nullptr, nullptr, nullptr, nullptr, true);
		addActor.send(Future<Void>(Void()));
		co_await collection;
		benchmark::DoNotOptimize(collection);
	}
	state->SetItemsProcessed(state->iterations());
	co_return;
}

Future<Void> benchActorCollectionBatchComplete(benchmark::State* state) {
	const int batchSize = state->range(0);

	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		int count = 0;
		Future<Void> collection = actorCollection(addActor.getFuture(), &count, nullptr, nullptr, nullptr, true);

		std::vector<Promise<Void>> signals(batchSize);
		for (auto& signal : signals) {
			addActor.send(signal.getFuture());
		}
		while (count < batchSize) {
			co_await yield();
		}

		for (auto& signal : signals) {
			signal.send(Void());
		}
		co_await collection;
		ASSERT_EQ(count, 0);
		benchmark::DoNotOptimize(collection);
	}

	state->SetItemsProcessed(batchSize * state->iterations());
	co_return;
}

Future<Void> benchActorCollectionBatchCancel(benchmark::State* state) {
	const int batchSize = state->range(0);
	uint64_t cleanupCount = 0;
	uint64_t cancelledCount = 0;
	uint64_t expectedCount = 0;

	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		state->PauseTiming();
		PromiseStream<Future<Void>> addActor;
		int count = 0;
		Future<Void> collection = actorCollection(addActor.getFuture(), &count);
		std::vector<Promise<Void>> signals(batchSize);
		for (auto& signal : signals) {
			addActor.send(cancellableFuture(signal.getFuture(), &cleanupCount, &cancelledCount));
		}
		while (count < batchSize) {
			co_await yield();
		}
		state->ResumeTiming();

		collection.cancel();
		benchmark::DoNotOptimize(collection);
		benchmark::ClobberMemory();

		state->PauseTiming();
		expectedCount += static_cast<uint64_t>(batchSize);
		while (cleanupCount < expectedCount) {
			co_await yield();
		}
		ASSERT_EQ(cancelledCount, expectedCount);
		state->ResumeTiming();
	}

	state->SetItemsProcessed(batchSize * state->iterations());
	co_return;
}

void bench_actorCollection_construct_cancel(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionConstructCancel(&state); }).blockUntilReady();
}

void bench_actorCollection_ready_child(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionReadyChild(&state); }).blockUntilReady();
}

void bench_actorCollection_batch_complete(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionBatchComplete(&state); }).blockUntilReady();
}

void bench_actorCollection_batch_cancel(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionBatchCancel(&state); }).blockUntilReady();
}

BENCHMARK(bench_actorCollection_construct_cancel)->Name("ActorCollection/construct_cancel")->ReportAggregatesOnly(true);

BENCHMARK(bench_actorCollection_ready_child)->Name("ActorCollection/ready_child")->ReportAggregatesOnly(true);

BENCHMARK(bench_actorCollection_batch_complete)
    ->Name("ActorCollection/batch_complete")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

BENCHMARK(bench_actorCollection_batch_cancel)
    ->Name("ActorCollection/batch_cancel")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

} // namespace
