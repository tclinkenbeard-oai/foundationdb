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
#include "genericcoros.h"

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

enum class ActorCollectionImpl { Actor, Coroutine };

template <ActorCollectionImpl Impl>
Future<Void> makeActorCollection(PromiseStream<Future<Void>>& addActor,
                                 int* count = nullptr,
                                 bool returnWhenEmptied = false) {
	if constexpr (Impl == ActorCollectionImpl::Actor) {
		return ::actorCollection(addActor.getFuture(), count, nullptr, nullptr, nullptr, returnWhenEmptied);
	} else {
		return generic_coro::actorCollection(addActor.getFuture(), count, nullptr, nullptr, nullptr, returnWhenEmptied);
	}
}

template <ActorCollectionImpl Impl>
Future<Void> benchActorCollectionConstructCancel(benchmark::State* state) {
	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		Future<Void> collection = makeActorCollection<Impl>(addActor);
		benchmark::DoNotOptimize(collection);
		collection.cancel();
		benchmark::DoNotOptimize(collection);
		benchmark::ClobberMemory();
	}
	state->SetItemsProcessed(state->iterations());
	co_return;
}

template <ActorCollectionImpl Impl>
Future<Void> benchActorCollectionReadyChild(benchmark::State* state) {
	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		Future<Void> collection = makeActorCollection<Impl>(addActor, nullptr, true);
		addActor.send(Future<Void>(Void()));
		co_await collection;
		benchmark::DoNotOptimize(collection);
	}
	state->SetItemsProcessed(state->iterations());
	co_return;
}

template <ActorCollectionImpl Impl>
Future<Void> benchActorCollectionErrorChild(benchmark::State* state) {
	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		Future<Void> collection = makeActorCollection<Impl>(addActor);
		addActor.send(Future<Void>(operation_failed()));
		try {
			co_await collection;
			ASSERT(false);
		} catch (Error& e) {
			ASSERT_EQ(e.code(), error_code_operation_failed);
		}
		benchmark::DoNotOptimize(collection);
	}
	state->SetItemsProcessed(state->iterations());
	co_return;
}

template <ActorCollectionImpl Impl>
Future<Void> benchActorCollectionBatchComplete(benchmark::State* state) {
	const int batchSize = state->range(0);

	for (auto _ : *state) {
		benchmark::DoNotOptimize(_);
		PromiseStream<Future<Void>> addActor;
		int count = 0;
		Future<Void> collection = makeActorCollection<Impl>(addActor, &count, true);

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

template <ActorCollectionImpl Impl>
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
		Future<Void> collection = makeActorCollection<Impl>(addActor, &count);
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

template <ActorCollectionImpl Impl>
void bench_actorCollection_construct_cancel(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionConstructCancel<Impl>(&state); }).blockUntilReady();
}

template <ActorCollectionImpl Impl>
void bench_actorCollection_ready_child(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionReadyChild<Impl>(&state); }).blockUntilReady();
}

template <ActorCollectionImpl Impl>
void bench_actorCollection_error_child(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionErrorChild<Impl>(&state); }).blockUntilReady();
}

template <ActorCollectionImpl Impl>
void bench_actorCollection_batch_complete(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionBatchComplete<Impl>(&state); }).blockUntilReady();
}

template <ActorCollectionImpl Impl>
void bench_actorCollection_batch_cancel(benchmark::State& state) {
	onMainThread([&state] { return benchActorCollectionBatchCancel<Impl>(&state); }).blockUntilReady();
}

BENCHMARK_TEMPLATE(bench_actorCollection_construct_cancel, ActorCollectionImpl::Actor)
    ->Name("ActorCollection/actor/construct_cancel")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_construct_cancel, ActorCollectionImpl::Coroutine)
    ->Name("ActorCollection/coroutine/construct_cancel")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_ready_child, ActorCollectionImpl::Actor)
    ->Name("ActorCollection/actor/ready_child")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_ready_child, ActorCollectionImpl::Coroutine)
    ->Name("ActorCollection/coroutine/ready_child")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_error_child, ActorCollectionImpl::Actor)
    ->Name("ActorCollection/actor/error_child")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_error_child, ActorCollectionImpl::Coroutine)
    ->Name("ActorCollection/coroutine/error_child")
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_batch_complete, ActorCollectionImpl::Actor)
    ->Name("ActorCollection/actor/batch_complete")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_batch_complete, ActorCollectionImpl::Coroutine)
    ->Name("ActorCollection/coroutine/batch_complete")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_batch_cancel, ActorCollectionImpl::Actor)
    ->Name("ActorCollection/actor/batch_cancel")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

BENCHMARK_TEMPLATE(bench_actorCollection_batch_cancel, ActorCollectionImpl::Coroutine)
    ->Name("ActorCollection/coroutine/batch_cancel")
    ->Range(1, 1 << 12)
    ->ReportAggregatesOnly(true);

} // namespace
