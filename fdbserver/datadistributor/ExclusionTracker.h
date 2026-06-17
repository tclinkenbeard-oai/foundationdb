/*
 * ExclusionTracker.h
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

#include <algorithm>
#include <set>

#include "flow/CoroUtils.h"
#include "flow/flow.h"
#include "flow/Trace.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/ManagementAPI.h"

struct ExclusionTracker {
	std::set<AddressExclusion> excluded;
	std::set<AddressExclusion> failed;

	AsyncTrigger changed;

	Database db;
	Future<Void> trackerFuture;

	ExclusionTracker() {}
	explicit ExclusionTracker(Database db) : db(db) { trackerFuture = tracker(this); }

	bool isFailedOrExcluded(NetworkAddress addr) {
		AddressExclusion addrExclusion(addr.ip, addr.port);
		return excluded.contains(addrExclusion) || failed.contains(addrExclusion);
	}

	static void addLocalityExclusions(LocalityData const& locality,
	                                  NetworkAddress const& address,
	                                  Optional<NetworkAddress> const& secondaryAddress,
	                                  std::vector<std::pair<std::string, std::string>> const& decodedExcludedLocalities,
	                                  std::vector<std::pair<std::string, std::string>> const& decodedFailedLocalities,
	                                  std::set<AddressExclusion>& excluded,
	                                  std::set<AddressExclusion>& failed) {
		auto addIfMatching = [&](std::vector<std::pair<std::string, std::string>> const& localities,
		                         std::set<AddressExclusion>& addresses) {
			if (std::none_of(localities.begin(), localities.end(), [&](auto const& excludedLocality) {
				    auto value = locality.get(excludedLocality.first);
				    return value.present() && value.get() == excludedLocality.second;
			    })) {
				return;
			}

			addresses.insert(AddressExclusion(address.ip, address.port));
			if (secondaryAddress.present()) {
				addresses.insert(AddressExclusion(secondaryAddress.get().ip, secondaryAddress.get().port));
			}
		};

		addIfMatching(decodedExcludedLocalities, excluded);
		addIfMatching(decodedFailedLocalities, failed);
	}

	static void addWorkerLocalityExclusions(
	    std::vector<ProcessData> const& workers,
	    std::vector<std::pair<std::string, std::string>> const& decodedExcludedLocalities,
	    std::vector<std::pair<std::string, std::string>> const& decodedFailedLocalities,
	    std::set<AddressExclusion>& excluded,
	    std::set<AddressExclusion>& failed) {
		for (auto const& worker : workers) {
			addLocalityExclusions(worker.locality,
			                      worker.address,
			                      Optional<NetworkAddress>(),
			                      decodedExcludedLocalities,
			                      decodedFailedLocalities,
			                      excluded,
			                      failed);
		}
	}

	// Locality exclusions must be resolved against both lists. The server list retains storage servers that are
	// temporarily down, while the worker list prevents the Data Distributor from recruiting a new storage server on
	// an excluded process after the old storage server has been removed.
	static Future<Void> tracker(ExclusionTracker* self) {
		// Fetch the list of excluded servers
		ReadYourWritesTransaction tr(self->db);
		while (true) {
			Error err;
			bool hasErr = false;
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
				Future<RangeResult> fresultsExclude = tr.getRange(excludedServersKeys, CLIENT_KNOBS->TOO_MANY);
				Future<RangeResult> fresultsFailed = tr.getRange(failedServersKeys, CLIENT_KNOBS->TOO_MANY);
				Future<RangeResult> flocalitiesExclude = tr.getRange(excludedLocalityKeys, CLIENT_KNOBS->TOO_MANY);
				Future<RangeResult> flocalitiesFailed = tr.getRange(failedLocalityKeys, CLIENT_KNOBS->TOO_MANY);
				Future<RangeResult> fServerList = tr.getRange(serverListKeys, CLIENT_KNOBS->TOO_MANY);
				Future<RangeResult> fWorkerList = tr.getRange(workerListKeys, CLIENT_KNOBS->TOO_MANY);

				co_await (success(fresultsExclude) && success(fresultsFailed) && success(flocalitiesExclude) &&
				          success(flocalitiesFailed) && success(fServerList) && success(fWorkerList));

				RangeResult excludedResults = fresultsExclude.get();
				ASSERT(!excludedResults.more && excludedResults.size() < CLIENT_KNOBS->TOO_MANY);

				RangeResult failedResults = fresultsFailed.get();
				ASSERT(!failedResults.more && failedResults.size() < CLIENT_KNOBS->TOO_MANY);

				RangeResult excludedLocalityResults = flocalitiesExclude.get();
				ASSERT(!excludedLocalityResults.more && excludedLocalityResults.size() < CLIENT_KNOBS->TOO_MANY);

				RangeResult failedLocalityResults = flocalitiesFailed.get();
				ASSERT(!failedLocalityResults.more && failedLocalityResults.size() < CLIENT_KNOBS->TOO_MANY);

				std::set<AddressExclusion> newExcluded;
				std::set<AddressExclusion> newFailed;
				for (const auto& r : excludedResults) {
					AddressExclusion addr = decodeExcludedServersKey(r.key);
					if (addr.isValid()) {
						newExcluded.insert(addr);
					}
				}
				for (const auto& r : failedResults) {
					AddressExclusion addr = decodeFailedServersKey(r.key);
					if (addr.isValid()) {
						newFailed.insert(addr);
					}
				}

				std::vector<std::pair<std::string, std::string>> decodedExcludedLocalities;
				for (auto& excludedLocality : excludedLocalityResults) {
					decodedExcludedLocalities.push_back(
					    decodeLocality(decodeExcludedLocalityKey(excludedLocality.key)));
				}

				std::vector<std::pair<std::string, std::string>> decodedFailedLocalities;
				for (auto& failedLocality : failedLocalityResults) {
					decodedFailedLocalities.push_back(decodeLocality(decodeFailedLocalityKey(failedLocality.key)));
				}

				RangeResult workerList = fWorkerList.get();
				ASSERT(!workerList.more && workerList.size() < CLIENT_KNOBS->TOO_MANY);
				std::vector<ProcessData> workers;
				workers.reserve(workerList.size());
				for (auto const& worker : workerList) {
					workers.push_back(decodeWorkerListValue(worker.value));
				}
				addWorkerLocalityExclusions(
				    workers, decodedExcludedLocalities, decodedFailedLocalities, newExcluded, newFailed);

				// A process that is down for maintenance is absent from the worker list but can still own storage.
				// Keep resolving the same localities against the server list as well.
				// See: https://github.com/apple/foundationdb/issues/12168
				RangeResult serverList = fServerList.get();
				ASSERT(!serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY);
				for (auto& s : serverList) {
					auto decodedServer = decodeServerListValue(s.value);
					auto addresses = decodedServer.getKeyValues.getEndpoint().addresses;
					addLocalityExclusions(decodedServer.locality,
					                      addresses.address,
					                      addresses.secondaryAddress,
					                      decodedExcludedLocalities,
					                      decodedFailedLocalities,
					                      newExcluded,
					                      newFailed);
				}

				bool foundChange = false;
				if (self->excluded != newExcluded) {
					self->excluded = newExcluded;
					foundChange = true;
				}
				if (self->failed != newFailed) {
					self->failed = newFailed;
					foundChange = true;
				}

				if (foundChange) {
					self->changed.trigger();
				}

				Future<Void> watchFuture = tr.watch(excludedServersVersionKey) || tr.watch(failedServersVersionKey) ||
				                           tr.watch(excludedLocalityVersionKey) || tr.watch(failedLocalityVersionKey);
				co_await tr.commit();
				if (excludedLocalityResults.size() > 0 || failedLocalityResults.size() > 0) {
					// when there are excluded localities we need to monitor for when the worker list changes, so we
					// must poll
					watchFuture = watchFuture || delay(10.0);
				}
				co_await watchFuture;
				tr.reset();
			} catch (Error& e) {
				err = e;
				hasErr = true;
			}
			if (hasErr) {
				TraceEvent("ExclusionTrackerError").error(err);
				co_await tr.onError(err);
			}
		}
	}
};
