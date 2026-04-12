/*
 * TaskPriority.h
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

#ifndef FLOW_TASKPRIORITY_H
#define FLOW_TASKPRIORITY_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>

#include "flow/Error.h"

enum class TaskPriority {
#define FLOW_TASK_PRIORITY_ITEM(name, value) name = value,
#include "flow/TaskPriorityItems.inc"
#undef FLOW_TASK_PRIORITY_ITEM
};

template <size_t N>
consteval std::array<TaskPriority, N> sortTaskPriorities(std::array<TaskPriority, N> priorities) {
	for (size_t i = 1; i < N; ++i) {
		TaskPriority key = priorities[i];
		size_t j = i;
		while (j > 0 && static_cast<int>(priorities[j - 1]) > static_cast<int>(key)) {
			priorities[j] = priorities[j - 1];
			--j;
		}
		priorities[j] = key;
	}
	return priorities;
}

// Keep this list sorted by numeric priority. Runtime integer priorities are normalized onto this set at the API
// boundaries so TaskPriority remains a closed set for internal bookkeeping.
inline constexpr auto knownTaskPriorities = sortTaskPriorities(std::array{
#define FLOW_TASK_PRIORITY_ITEM(name, value) TaskPriority::name,
#include "flow/TaskPriorityItems.inc"
#undef FLOW_TASK_PRIORITY_ITEM
});

static_assert(std::is_sorted(knownTaskPriorities.begin(),
                             knownTaskPriorities.end(),
                             [](TaskPriority lhs, TaskPriority rhs) {
	                             return static_cast<int>(lhs) < static_cast<int>(rhs);
                             }));
inline constexpr int getKnownTaskPriorityIndex(TaskPriority priority) {
	const auto it = std::lower_bound(
	    knownTaskPriorities.begin(),
	    knownTaskPriorities.end(),
	    static_cast<int>(priority),
	    [](TaskPriority knownPriority, int rawPriority) { return static_cast<int>(knownPriority) < rawPriority; });
	if (it == knownTaskPriorities.end() || *it != priority) {
		return -1;
	}
	return static_cast<int>(std::distance(knownTaskPriorities.begin(), it));
}

inline constexpr TaskPriority normalizeTaskPriorityDown(TaskPriority priority) {
	const auto it = std::upper_bound(
	    knownTaskPriorities.begin(),
	    knownTaskPriorities.end(),
	    static_cast<int>(priority),
	    [](int rawPriority, TaskPriority knownPriority) { return rawPriority < static_cast<int>(knownPriority); });
	if (it == knownTaskPriorities.begin()) {
		return *it;
	}
	return *std::prev(it);
}

inline constexpr TaskPriority normalizeTaskPriorityUp(TaskPriority priority) {
	const auto it = std::lower_bound(
	    knownTaskPriorities.begin(),
	    knownTaskPriorities.end(),
	    static_cast<int>(priority),
	    [](TaskPriority knownPriority, int rawPriority) { return static_cast<int>(knownPriority) < rawPriority; });
	if (it == knownTaskPriorities.end()) {
		return knownTaskPriorities.back();
	}
	return *it;
}

// These have been given long, annoying names to discourage their use.

inline TaskPriority incrementPriority(TaskPriority p) {
	p = normalizeTaskPriorityDown(p);
	const int rawPriority = static_cast<int>(p);
	auto next = normalizeTaskPriorityUp(static_cast<TaskPriority>(rawPriority + 1));
	ASSERT(getKnownTaskPriorityIndex(next) >= 0);
	return next;
}

inline TaskPriority decrementPriority(TaskPriority p) {
	p = normalizeTaskPriorityDown(p);
	const int rawPriority = static_cast<int>(p);
	auto prev = normalizeTaskPriorityDown(static_cast<TaskPriority>(rawPriority - 1));
	ASSERT(getKnownTaskPriorityIndex(prev) >= 0);
	return prev;
}

inline TaskPriority incrementPriorityIfEven(TaskPriority p) {
	p = normalizeTaskPriorityDown(p);
	const int rawPriority = static_cast<int>(p);
	auto next = normalizeTaskPriorityUp(static_cast<TaskPriority>(rawPriority | 1));
	ASSERT(getKnownTaskPriorityIndex(next) >= 0);
	return next;
}

inline TaskPriority getTaskPriorityFromInt(int p) {
	ASSERT(p >= static_cast<int>(TaskPriority::Min) && p <= static_cast<int>(TaskPriority::Max));
	// Integer-configured priorities may land between named enum values. Normalize them here so the runtime only
	// schedules known TaskPriority levels and internal tracker storage can stay fixed-size.
	auto normalized = normalizeTaskPriorityDown(static_cast<TaskPriority>(p));
	ASSERT(getKnownTaskPriorityIndex(normalized) >= 0);
	return normalized;
}

#endif // FLOW_TASKPRIORITY_H
