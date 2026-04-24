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

struct Runner : public boost::intrusive::list_base_hook<>, Callback<Void>, FastAllocated<Runner>, NonCopyable {
	PromiseStream<Runner*> output;
	PromiseStream<Error> errors;
	bool callbackRegistered = false;

	Runner(PromiseStream<Runner*> output, PromiseStream<Error> errors)
	  : output(std::move(output)), errors(std::move(errors)) {}

	~Runner() { removeCallback(); }

	void start(Future<Void> task) {
		if (task.isReady()) {
			if (task.isError()) {
				Error e = task.getError();
				if (e.code() != error_code_actor_cancelled) {
					errors.send(e);
				}
			} else {
				output.send(this);
			}
			return;
		}

		callbackRegistered = true;
		task.addCallbackAndClear(this);
	}

	void removeCallback() {
		if (callbackRegistered) {
			callbackRegistered = false;
			Callback<Void>::remove();
		}
	}

	void fire(Void const&) override {
		auto output = this->output;
		removeCallback();
		output.send(this);
	}

	void error(Error e) override {
		auto errors = this->errors;
		removeCallback();
		if (e.code() != error_code_actor_cancelled) {
			errors.send(e);
		}
	}
};

using RunnerList = boost::intrusive::list<Runner, boost::intrusive::constant_time_size<false>>;

struct RunnerListDestroyer : NonCopyable {
	explicit RunnerListDestroyer(RunnerList* list) : list(list) {}

	~RunnerListDestroyer() {
		list->clear_and_dispose([](Runner* r) { delete r; });
	}

	RunnerList* list;
};

struct Event {
	enum class Kind { Add, Complete, Error };

	Kind kind;
	Future<Void> actor;
	Runner* runner;
	Error err;

	static Event add(Future<Void> actor) { return Event{ Kind::Add, std::move(actor), nullptr, Error() }; }
	static Event complete(Runner* runner) { return Event{ Kind::Complete, Future<Void>(), runner, Error() }; }
	static Event error(Error err) { return Event{ Kind::Error, Future<Void>(), nullptr, err }; }
};

struct ActorCollectionEventAwaitable {
	using PromiseBoundAwaitableTag = void;

	FutureStream<Future<Void>> addActor;
	FutureStream<Runner*> complete;
	FutureStream<Error> errors;

	ActorCollectionEventAwaitable(FutureStream<Future<Void>> addActor,
	                              FutureStream<Runner*> complete,
	                              FutureStream<Error> errors)
	  : addActor(std::move(addActor)), complete(std::move(complete)), errors(std::move(errors)) {}

	template <class PromiseType>
	struct Bound final : ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>,
	                     ActorSingleCallback<Bound<PromiseType>, 1, Runner*>,
	                     ActorSingleCallback<Bound<PromiseType>, 2, Error>,
	                     coro::AwaitCancelHandler {
		FutureStream<Future<Void>> addActor;
		FutureStream<Runner*> complete;
		FutureStream<Error> errors;
		PromiseType* pt = nullptr;
		Event event;
		bool callbacksRegistered = false;

		Bound(FutureStream<Future<Void>> addActor,
		      FutureStream<Runner*> complete,
		      FutureStream<Error> errors,
		      PromiseType* pt)
		  : addActor(std::move(addActor)), complete(std::move(complete)), errors(std::move(errors)), pt(pt) {}

		bool await_ready() {
			if (actorWaitStateIsCancelled(pt->waitState())) {
				pt->waitState() = ACTOR_WAIT_STATE_CANCELLED_DURING_READY_CHECK;
				return true;
			}
			return setReadyEvent();
		}

		void await_suspend(n_coroutine::coroutine_handle<> h) {
			pt->setHandle(h);
			pt->waitState() = ACTOR_WAIT_STATE_WAITING;
			registerCallbacks();
			pt->setCancelHandler(this);
		}

		Event await_resume() {
			pt->clearCancelHandler(this);
			switch (pt->waitState()) {
			case ACTOR_WAIT_STATE_CANCELLED:
				cancelWait();
			case ACTOR_WAIT_STATE_CANCELLED_DURING_READY_CHECK:
				throw actor_cancelled();
			}

			if (actorWaitStateIsWaiting(pt->waitState())) {
				removeCallbacks();
				pt->waitState() = ACTOR_WAIT_STATE_NOT_WAITING;
			}
			return std::move(event);
		}

		void cancelWait() override { removeCallbacks(); }

		bool setReadyEvent() {
			if (addActor.isReady()) {
				event = Event::add(addActor.pop());
				return true;
			}
			if (complete.isReady()) {
				event = Event::complete(complete.pop());
				return true;
			}
			if (errors.isReady()) {
				event = Event::error(errors.pop());
				return true;
			}
			return false;
		}

		void registerCallbacks() {
			callbacksRegistered = true;
			auto addActorStream = addActor;
			addActorStream.addCallbackAndClear(
			    static_cast<ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>*>(this));
			auto completeStream = complete;
			completeStream.addCallbackAndClear(static_cast<ActorSingleCallback<Bound<PromiseType>, 1, Runner*>*>(this));
			auto errorsStream = errors;
			errorsStream.addCallbackAndClear(static_cast<ActorSingleCallback<Bound<PromiseType>, 2, Error>*>(this));
		}

		void removeCallbacks() {
			if (!callbacksRegistered) {
				return;
			}
			callbacksRegistered = false;
			static_cast<ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>*>(this)->remove();
			static_cast<ActorSingleCallback<Bound<PromiseType>, 1, Runner*>*>(this)->remove();
			static_cast<ActorSingleCallback<Bound<PromiseType>, 2, Error>*>(this)->remove();
		}

		void finish(Event readyEvent) {
			event = std::move(readyEvent);
			pt->resume();
		}

		void fail(Error e) {
			event = Event::error(e);
			pt->resume();
		}

		void a_callback_fire(ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>*, Future<Void> const& actor) {
			finish(Event::add(actor));
		}
		void a_callback_fire(ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>*, Future<Void>&& actor) {
			finish(Event::add(std::move(actor)));
		}
		void a_callback_error(ActorSingleCallback<Bound<PromiseType>, 0, Future<Void>>*, Error e) { fail(e); }

		void a_callback_fire(ActorSingleCallback<Bound<PromiseType>, 1, Runner*>*, Runner* const& runner) {
			finish(Event::complete(runner));
		}
		void a_callback_error(ActorSingleCallback<Bound<PromiseType>, 1, Runner*>*, Error e) { fail(e); }

		void a_callback_fire(ActorSingleCallback<Bound<PromiseType>, 2, Error>*, Error const& e) {
			finish(Event::error(e));
		}
		void a_callback_error(ActorSingleCallback<Bound<PromiseType>, 2, Error>*, Error e) { fail(e); }
	};

	template <class PromiseType>
	Bound<PromiseType> bindPromise(PromiseType* pt) && {
		return Bound<PromiseType>(std::move(addActor), std::move(complete), std::move(errors), pt);
	}
};

inline ActorCollectionEventAwaitable actorCollectionEvent(FutureStream<Future<Void>> addActor,
                                                          FutureStream<Runner*> complete,
                                                          FutureStream<Error> errors) {
	return ActorCollectionEventAwaitable(std::move(addActor), std::move(complete), std::move(errors));
}

inline Future<Void> actorCollectionNonEmptyImpl(FutureStream<Future<Void>> addActor,
                                                Future<Void> firstActor,
                                                int* pCount,
                                                double* lastChangeTime,
                                                double* idleTime,
                                                double* allTime,
                                                bool returnWhenEmptied) {
	RunnerList runners;
	RunnerListDestroyer runnersDestroyer(&runners);
	PromiseStream<Runner*> complete;
	PromiseStream<Error> errors;
	int count = 0;
	if (!pCount) {
		pCount = &count;
	}

	auto addRunner = [&](Future<Void> f) {
		auto i = runners.insert(runners.end(), *new Runner(complete, errors));
		i->start(std::move(f));

		++*pCount;
		if (*pCount == 1 && lastChangeTime && idleTime && allTime) {
			double currentTime = now();
			*idleTime += currentTime - *lastChangeTime;
			*allTime += currentTime - *lastChangeTime;
			*lastChangeTime = currentTime;
		}
	};

	auto addOnlyRunnerOrReady = [&](Future<Void> f) {
		if (f.isReady()) {
			if (f.isError()) {
				Error e = f.getError();
				if (e.code() == error_code_actor_cancelled) {
					addRunner(std::move(f));
					return false;
				}

				++*pCount;
				if (*pCount == 1 && lastChangeTime && idleTime && allTime) {
					double currentTime = now();
					*idleTime += currentTime - *lastChangeTime;
					*allTime += currentTime - *lastChangeTime;
					*lastChangeTime = currentTime;
				}
				throw e;
			}

			++*pCount;
			if (*pCount == 1 && lastChangeTime && idleTime && allTime) {
				double currentTime = now();
				*idleTime += currentTime - *lastChangeTime;
				*allTime += currentTime - *lastChangeTime;
				*lastChangeTime = currentTime;
			}
			if (!--*pCount) {
				if (lastChangeTime && idleTime && allTime) {
					double currentTime = now();
					*allTime += currentTime - *lastChangeTime;
					*lastChangeTime = currentTime;
				}
				return returnWhenEmptied;
			}
			return false;
		}

		addRunner(std::move(f));
		return false;
	};

	if (addOnlyRunnerOrReady(std::move(firstActor))) {
		co_return;
	}

	while (true) {
		if (runners.empty()) {
			if (addOnlyRunnerOrReady(co_await addActor)) {
				co_return;
			}
			continue;
		}

		Event event = co_await actorCollectionEvent(addActor, complete.getFuture(), errors.getFuture());
		if (event.kind == Event::Kind::Add) {
			addRunner(std::move(event.actor));
		} else if (event.kind == Event::Kind::Complete) {
			Runner* runner = event.runner;
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
			runners.erase_and_dispose(runners.iterator_to(*runner), [](Runner* r) { delete r; });
		} else {
			throw event.err;
		}
	}
}

inline Future<Void> actorCollectionImpl(FutureStream<Future<Void>> addActor,
                                        int* pCount,
                                        double* lastChangeTime,
                                        double* idleTime,
                                        double* allTime,
                                        bool returnWhenEmptied,
                                        NoThrowOnCancel = {}) {
	Future<Void> firstActor = co_await addActor;
	co_await actorCollectionNonEmptyImpl(
	    addActor, std::move(firstActor), pCount, lastChangeTime, idleTime, allTime, returnWhenEmptied);
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
