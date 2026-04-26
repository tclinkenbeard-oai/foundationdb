/*
 * ActorCollection.actor.cpp
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

#include "flow/ActorCollection.h"
#include "flow/Coroutines.h"
#include "flow/IndexedSet.h"
#include "flow/UnitTest.h"
#include <boost/intrusive/list.hpp>
#include "flow/actorcompiler.h" // This must be the last #include.

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

// The runners list in the ActorCollection must be destroyed when the actor is destructed rather
// than before returning or throwing
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
	struct Bound final : SingleCallback<Future<Void>>,
	                     SingleCallback<Runner*>,
	                     SingleCallback<Error>,
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
			addActorStream.addCallbackAndClear(static_cast<SingleCallback<Future<Void>>*>(this));
			auto completeStream = complete;
			completeStream.addCallbackAndClear(static_cast<SingleCallback<Runner*>*>(this));
			auto errorsStream = errors;
			errorsStream.addCallbackAndClear(static_cast<SingleCallback<Error>*>(this));
		}

		void removeCallbacks() {
			if (!callbacksRegistered) {
				return;
			}
			callbacksRegistered = false;
			static_cast<SingleCallback<Future<Void>>*>(this)->remove();
			static_cast<SingleCallback<Runner*>*>(this)->remove();
			static_cast<SingleCallback<Error>*>(this)->remove();
		}

		void finish(Event readyEvent) {
			event = std::move(readyEvent);
			pt->resume();
		}

		void fail(Error e) {
			event = Event::error(e);
			pt->resume();
		}

		void fire(Future<Void> const& actor) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::add(actor));
		}
		void fire(Future<Void>&& actor) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::add(std::move(actor)));
		}
		void error(Error e) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			fail(e);
		}

		void fire(Runner* const& runner) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::complete(runner));
		}
		void fire(Runner*&& runner) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::complete(runner));
		}

		void fire(Error const& e) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::error(e));
		}
		void fire(Error&& e) override {
#ifdef ENABLE_SAMPLING
			LineageScope _(currentLineage);
#endif
			finish(Event::error(e));
		}
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

Future<Void> actorCollection(FutureStream<Future<Void>> const& addActor,
                             int* const& optionalCountPtr,
                             double* const& lastChangeTime,
                             double* const& idleTime,
                             double* const& allTime,
                             bool const& returnWhenEmptied) {
	return actor_collection_detail::actorCollectionImpl(
	    addActor, optionalCountPtr, lastChangeTime, idleTime, allTime, returnWhenEmptied);
}

TEST_CASE("/flow/actorCollection/readyChildReturnsWhenEmptied") {
	state PromiseStream<Future<Void>> addActor;
	state int count = 0;
	state Future<Void> collection = actorCollection(addActor.getFuture(), &count, nullptr, nullptr, nullptr, true);

	addActor.send(Void());
	wait(collection);
	ASSERT_EQ(count, 0);

	return Void();
}

TEST_CASE("/flow/actorCollection/readyChildWhileNonEmpty") {
	state PromiseStream<Future<Void>> addActor;
	state Promise<Void> pending;
	state int count = 0;
	state Future<Void> collection = actorCollection(addActor.getFuture(), &count, nullptr, nullptr, nullptr, true);

	addActor.send(pending.getFuture());
	wait(delay(0));
	ASSERT_EQ(count, 1);

	addActor.send(Void());
	wait(delay(0));
	ASSERT_EQ(count, 1);
	ASSERT(!collection.isReady());

	pending.send(Void());
	wait(collection);
	ASSERT_EQ(count, 0);

	return Void();
}

template <class T, class U>
struct Traceable<std::pair<T, U>> {
	static constexpr bool value = Traceable<T>::value && Traceable<U>::value;
	static std::string toString(const std::pair<T, U>& p) {
		auto tStr = Traceable<T>::toString(p.first);
		auto uStr = Traceable<U>::toString(p.second);
		std::string result(tStr.size() + uStr.size() + 3, 'x');
		std::copy(tStr.begin(), tStr.end(), result.begin());
		auto iter = result.begin() + tStr.size();
		*(iter++) = ' ';
		*(iter++) = '-';
		*(iter++) = ' ';
		std::copy(uStr.begin(), uStr.end(), iter);
		return result;
	}
};

void forceLinkActorCollectionTests() {}

// The above implementation relies on the behavior that fulfilling a promise
// that another when clause in the same choose block is waiting on is not fired synchronously.
TEST_CASE("/flow/actorCollection/chooseWhen") {
	state Promise<Void> promise;
	choose {
		when(wait(delay(0))) {
			promise.send(Void());
		}
		when(wait(promise.getFuture())) {
			// Should be cancelled, since another when clause in this choose block has executed
			ASSERT(false);
		}
	}
	return Void();
}

ACTOR Future<Void> failIfNotCancelled() {
	wait(delay(0));
	ASSERT(false);
	return Void();
}

// test contract that actors are cancelled when the actor collection is cleared
TEST_CASE("/flow/actorCollection/testCancel") {
	state ActorCollection actorCollection(false);
	int actors = deterministicRandom()->randomInt(1, 1000);
	for (int i = 0; i < actors; i++) {
		actorCollection.add(failIfNotCancelled());
	}
	actorCollection.clear(false);
	wait(delay(0));
	return Void();
}

Future<Void> failedActor() {
	return operation_failed();
}

TEST_CASE("/flow/actorCollection/errorPropagation") {
	state ActorCollection actorCollection(false);

	actorCollection.add(failedActor());
	try {
		wait(actorCollection.getResult());
		ASSERT(false);
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_operation_failed);
	}

	return Void();
}

// test contract that even if the actor collection has stopped and new actors are added to the promise stream, they are
// all cancelled when resetting actor
TEST_CASE("/flow/actorCollection/testCancelPromiseStream") {
	state ActorCollection actorCollection(false);
	int actors = deterministicRandom()->randomInt(1, 500);
	for (int i = 0; i < actors; i++) {
		actorCollection.add(failIfNotCancelled());
	}
	// this actor should cause the actorCollection actor to exit, meaning the new futures just build up in the promise
	// stream
	actorCollection.add(failedActor());
	for (int i = 0; i < actors; i++) {
		actorCollection.add(failIfNotCancelled());
	}
	// Instead of doing actorCollection.clear(false) we reinitialize to also clear the promise stream. Otherwise on
	// resetting the actor collection actor, the new actors will be pulled from the promise stream into the new instance
	// Note that this test fails on the assert in failIfNotCancelled() when this is replaced with
	// actorCollection.clear(false).
	actorCollection = ActorCollection(false);
	wait(delay(0));
	return Void();
}
