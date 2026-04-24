/*
 * genericcoros.h
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

#include "flow/Coroutines.h"
#include "flow/flow.h"

#include <boost/intrusive/list.hpp>
#include <vector>

namespace generic_coro {

template <class T>
Future<T> traceAfter(Future<T> what, std::string eventType, bool traceErrors = true, ExplicitVoid = {}) {
	try {
		T val = co_await what;
		TraceEvent(eventType.c_str());
		co_return val;
	} catch (Error& e) {
		// Don't trace operation_cancelled as it's a normal control flow mechanism, not an error
		if (traceErrors && e.code() != error_code_operation_cancelled) {
			TraceEvent(eventType.c_str()).errorUnsuppressed(e);
		}
		throw;
	}
}

template <class T>
Future<Optional<T>> stopAfter(Future<T> what, ExplicitVoid = {}) {
	Optional<T> ret = T();
	try {
		T res = co_await what;
		ret = Optional<T>(res);
	} catch (Error& e) {
		bool ok = e.code() == error_code_please_reboot || e.code() == error_code_please_reboot_delete ||
		          e.code() == error_code_actor_cancelled || e.code() == error_code_local_config_changed;
		TraceEvent(ok ? SevInfo : SevError, "StopAfterError").error(e);
		if (!ok) {
			fprintf(stderr, "Fatal Error: %s\n", e.what());
			ret = Optional<T>();
		}
	}
	g_network->stop();
	co_return ret;
}

template <class T>
Future<T> throwErrorOr(Future<ErrorOr<T>> f, ExplicitVoid = {}) {
	ErrorOr<T> t = co_await f;
	if (t.isError()) {
		throw t.getError();
	}
	co_return std::move(t).get();
}

template <class T>
Future<T> transformErrors(Future<T> f, Error err, ExplicitVoid = {}) {
	ErrorOr<T> t = co_await coro::errorOr(f);
	if (t.present()) {
		co_return std::move(t).get();
	}
	Error e = t.getError();
	if (e.code() == error_code_actor_cancelled) {
		throw e;
	}
	throw err;
}

template <class T>
Future<T> transformError(Future<T> f, Error inErr, Error outErr, ExplicitVoid = {}) {
	ErrorOr<T> t = co_await coro::errorOr(f);
	if (t.present()) {
		co_return std::move(t).get();
	}
	Error e = t.getError();
	if (e.code() == inErr.code()) {
		throw outErr;
	}
	throw e;
}

template <class T>
Future<Void> waitForAllReady(std::vector<Future<T>> results) {
	for (auto const& result : results) {
		if (result.isReady()) {
			continue;
		}
		// waitForAllReady only cares that each future completes; composing
		// ignore() with errorOr() avoids both throwing on error and copying T.
		co_await coro::errorOr(coro::ignore(result));
	}
}

template <class T>
Future<T> timeout(Future<T> what,
                  double time,
                  T timedoutValue,
                  TaskPriority taskID = TaskPriority::DefaultDelay,
                  ExplicitVoid = {}) {
	if (what.canGet()) {
		co_return what.get();
	} else if (what.isError()) {
		throw what.getError();
	}
	auto res = co_await race(what, delay(time, taskID));
	if (res.index() == 0) {
		co_return std::get<0>(std::move(res));
	} else {
		co_return timedoutValue;
	}
}

template <class T>
Future<Optional<T>> timeout(Future<T> what,
                            double time,
                            TaskPriority taskID = TaskPriority::DefaultDelay,
                            ExplicitVoid = {}) {
	if (what.canGet()) {
		co_return what.get();
	} else if (what.isError()) {
		throw what.getError();
	}
	auto res = co_await race(what, delay(time, taskID));
	if (res.index() == 0) {
		co_return std::get<0>(std::move(res));
	} else {
		co_return Optional<T>();
	}
}

template <class T>
Future<T> timeoutError(Future<T> what,
                       double time,
                       TaskPriority taskID = TaskPriority::DefaultDelay,
                       ExplicitVoid = {}) {
	if (what.canGet()) {
		co_return what.get();
	} else if (what.isError()) {
		throw what.getError();
	}
	auto res = co_await race(what, delay(time, taskID));
	if (res.index() == 0) {
		co_return std::get<0>(std::move(res));
	} else {
		throw timed_out();
	}
}

template <class T>
Future<T> delayed(Future<T> what,
                  double time = 0.0,
                  TaskPriority taskID = TaskPriority::DefaultDelay,
                  ExplicitVoid = {}) {
	ErrorOr<T> t = co_await coro::errorOr(what);
	co_await delay(time, taskID);
	if (t.present()) {
		co_return std::move(t).get();
	} else {
		throw t.getError();
	}
}

template <class Func>
Future<Void> trigger(Func what, Future<Void> signal) {
	co_await signal;
	what();
}

template <class T>
Future<Void> uncancellable(Uncancellable, Future<T> what, Promise<T> result) {
	ErrorOr<T> res = co_await coro::errorOr(what);
	if (res.present()) {
		result.send(std::move(res).get());
	} else {
		result.sendError(res.getError());
	}
}

template <class T>
Future<T> uncancellable(Future<T> what, ExplicitVoid = {}) {
	Promise<T> resultPromise;
	Future<T> result = resultPromise.getFuture();

	uncancellable(Uncancellable(), what, resultPromise);
	co_return co_await result;
}

template <class T, class X>
Future<T> holdWhile(X object, Future<T> what) {
	co_return co_await what;
}

template <class T, class X>
Future<Void> store(X& out, Future<T> what) {
	out = co_await what;
}

namespace actor_collection_detail {

struct Runner : public boost::intrusive::list_base_hook<>, FastAllocated<Runner>, NonCopyable {
	Future<Void> handler;
};

using RunnerList = boost::intrusive::list<Runner, boost::intrusive::constant_time_size<false>>;

struct RunnerListDestroyer : NonCopyable {
	explicit RunnerListDestroyer(RunnerList* list) : list(list) {}

	~RunnerListDestroyer() {
		list->clear_and_dispose([](Runner* r) { delete r; });
	}

	RunnerList* list;
};

inline Future<Void> runnerHandler(PromiseStream<RunnerList::iterator> output,
                                  PromiseStream<Error> errors,
                                  Future<Void> task,
                                  RunnerList::iterator runner) {
	try {
		co_await task;
		output.send(runner);
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw;
		}
		errors.send(e);
	}
}

inline Future<Void> actorCollectionImpl(FutureStream<Future<Void>> addActor,
                                        int* pCount,
                                        double* lastChangeTime,
                                        double* idleTime,
                                        double* allTime,
                                        bool returnWhenEmptied) {
	RunnerList runners;
	RunnerListDestroyer runnersDestroyer(&runners);
	PromiseStream<RunnerList::iterator> complete;
	PromiseStream<Error> errors;
	int count = 0;
	if (!pCount) {
		pCount = &count;
	}

	while (true) {
		auto result = co_await race(addActor, complete.getFuture(), errors.getFuture());
		if (result.index() == 0) {
			Future<Void> f = std::get<0>(std::move(result));
			auto i = runners.insert(runners.end(), *new Runner());
			Future<Void> handler = runnerHandler(complete, errors, f, i);
			i->handler = handler;

			++*pCount;
			if (*pCount == 1 && lastChangeTime && idleTime && allTime) {
				double currentTime = now();
				*idleTime += currentTime - *lastChangeTime;
				*allTime += currentTime - *lastChangeTime;
				*lastChangeTime = currentTime;
			}
		} else if (result.index() == 1) {
			auto i = std::get<1>(std::move(result));
			if (!--*pCount) {
				if (lastChangeTime && idleTime && allTime) {
					double currentTime = now();
					*allTime += currentTime - *lastChangeTime;
					*lastChangeTime = currentTime;
				}
				if (returnWhenEmptied) {
					co_return;
				}
			}
			runners.erase_and_dispose(i, [](Runner* r) { delete r; });
		} else {
			throw std::get<2>(std::move(result));
		}
	}
}

} // namespace actor_collection_detail

inline Future<Void> actorCollection(FutureStream<Future<Void>> const& addActor,
                                    int* const& optionalCountPtr = nullptr,
                                    double* const& lastChangeTime = nullptr,
                                    double* const& idleTime = nullptr,
                                    double* const& allTime = nullptr,
                                    bool const& returnWhenEmptied = false) {
	return actor_collection_detail::actorCollectionImpl(
	    addActor, optionalCountPtr, lastChangeTime, idleTime, allTime, returnWhenEmptied);
}

struct ActorCollectionNoErrors : NonCopyable {
private:
	Future<Void> m_ac;
	PromiseStream<Future<Void>> m_add;
	int m_size;
	void init() {
		m_size = 0;
		m_ac = generic_coro::actorCollection(m_add.getFuture(), &m_size);
	}

public:
	ActorCollectionNoErrors() { init(); }
	void clear() {
		m_ac = Future<Void>();
		init();
	}
	void add(Future<Void> actor) { m_add.send(actor); }
	int size() const { return m_size; }
};

class ActorCollection : NonCopyable {
	PromiseStream<Future<Void>> m_add;
	Future<Void> m_out;

public:
	explicit ActorCollection(bool returnWhenEmptied = false) {
		m_out = generic_coro::actorCollection(m_add.getFuture(), nullptr, nullptr, nullptr, nullptr, returnWhenEmptied);
	}

	void add(Future<Void> a) { m_add.send(a); }
	Future<Void> getResult() const { return m_out; }
	void clear(bool returnWhenEmptied) {
		m_out.cancel();
		m_out = generic_coro::actorCollection(m_add.getFuture(), nullptr, nullptr, nullptr, nullptr, returnWhenEmptied);
	}
};

class SignalableActorCollection : NonCopyable {
	PromiseStream<Future<Void>> m_add;
	Promise<Void> stopSignal;
	Future<Void> m_out;

	void init() {
		PromiseStream<Future<Void>> addStream;
		m_out = generic_coro::actorCollection(addStream.getFuture(), nullptr, nullptr, nullptr, nullptr, true);
		m_add = addStream;
		stopSignal = Promise<Void>();
		m_add.send(stopSignal.getFuture());
	}

public:
	explicit SignalableActorCollection() { init(); }

	Future<Void> signal() {
		stopSignal.send(Void());
		Future<Void> result = generic_coro::holdWhile(m_add, m_out);
		return result;
	}

	Future<Void> signalAndReset() {
		Future<Void> result = signal();
		clear();
		return result;
	}

	Future<Void> signalAndCollapse() {
		Future<Void> result = signalAndReset();
		add(result);
		return result;
	}

	void add(Future<Void> a) { m_add.send(a); }
	void clear() { init(); }
};

} // namespace generic_coro
