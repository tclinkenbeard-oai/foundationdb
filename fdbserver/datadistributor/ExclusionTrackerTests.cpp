/*
 * ExclusionTrackerTests.cpp
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

#include "ExclusionTracker.h"
#include "flow/UnitTest.h"

void forceLinkExclusionTrackerTests() {}

TEST_CASE("/DataDistribution/ExclusionTracker/WorkerLocalityExclusions") {
	LocalityData matchingLocality;
	matchingLocality.set(LocalityData::keyProcessId, "excluded-process"_sr);
	matchingLocality.set(LocalityData::keyZoneId, "failed-zone"_sr);

	LocalityData otherLocality;
	otherLocality.set(LocalityData::keyProcessId, "other-process"_sr);
	otherLocality.set(LocalityData::keyZoneId, "other-zone"_sr);

	NetworkAddress matchingAddress = NetworkAddress::parse("1.2.3.4:4500");
	NetworkAddress otherAddress = NetworkAddress::parse("1.2.3.5:4500");
	std::vector<ProcessData> workers{
		ProcessData(matchingLocality, ProcessClass(), matchingAddress, Optional<NetworkAddress>()),
		ProcessData(otherLocality, ProcessClass(), otherAddress, Optional<NetworkAddress>()),
	};
	std::vector<std::pair<std::string, std::string>> excludedLocalities{ { "processid", "excluded-process" } };
	std::vector<std::pair<std::string, std::string>> failedLocalities{ { "zoneid", "failed-zone" } };
	std::set<AddressExclusion> excluded;
	std::set<AddressExclusion> failed;

	ExclusionTracker::addWorkerLocalityExclusions(workers, excludedLocalities, failedLocalities, excluded, failed);

	AddressExclusion matchingExclusion(matchingAddress.ip, matchingAddress.port);
	AddressExclusion otherExclusion(otherAddress.ip, otherAddress.port);
	ASSERT(excluded == std::set<AddressExclusion>{ matchingExclusion });
	ASSERT(failed == std::set<AddressExclusion>{ matchingExclusion });
	ASSERT(!excluded.contains(otherExclusion));
	ASSERT(!failed.contains(otherExclusion));
	return Void();
}
