#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_LOADSHARING_DISCOVERY)
#undef ENABLE_DEBUG
#endif

#include "debug.h"
#include "loadsharing_discovery_task.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <mdns.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Global instance
LoadSharingDiscoveryTask loadSharingDiscoveryTask;

LoadSharingDiscoveryTask::LoadSharingDiscoveryTask(unsigned long cacheTtl,
                                                   unsigned long poll_interval_ms,
                                                   unsigned long discovery_interval_ms,
                                                   unsigned long query_timeout_ms)
  : MicroTasks::Task(),
    _cacheTtl(cacheTtl),
    _lastDiscovery(0),
    _cacheValid(false),
    _groupPeersDirty(false),
    _poll_interval_ms(poll_interval_ms),
    _discovery_interval_ms(discovery_interval_ms),
    _query_timeout_ms(query_timeout_ms),
    _last_discovery_time(0),
    _active_query(nullptr),
    _query_start_time(0),
    _query_in_progress(false),
    _discovery_count(0),
    _last_result_count(0)
{
}

void LoadSharingDiscoveryTask::setup() {
  // Task is ready to run
  DBUGF("LoadSharingDiscoveryTask: Setup complete");
}

unsigned long LoadSharingDiscoveryTask::loop(MicroTasks::WakeReason reason) {
  unsigned long now = millis();

  // If a query is currently in progress, poll its status
  if (_query_in_progress) {
    if (pollAsyncQuery()) {
      // Query completed, process results handled in pollAsyncQuery()
      _query_in_progress = false;
      _active_query = nullptr;
    } else {
      // Query still in progress, check timeout
      if (now - _query_start_time > _query_timeout_ms) {
        // Query timed out
        DBUGF("LoadSharingDiscoveryTask: Query timeout after %lu ms", _query_timeout_ms);
        cleanupQuery();
        _query_in_progress = false;
      }
    }
  } else {
    // No query in progress, check if we should start a new one
    if (now - _last_discovery_time >= _discovery_interval_ms || _last_discovery_time == 0) {
      // Time to start a new discovery
      DBUGF("LoadSharingDiscoveryTask: Starting discovery iteration %lu", _discovery_count + 1);
      startAsyncQuery();
      _last_discovery_time = now;
      _discovery_count++;
    }
  }

  // Always wake up at next poll interval
  return _poll_interval_ms;
}

void LoadSharingDiscoveryTask::begin() {
  _lastDiscovery = 0;  // Invalidate cache
  _last_discovery_time = 0;  // Force immediate first discovery

  // Load group peers from storage
  loadGroupPeers();

  MicroTask.startTask(this);
  DBUGF("LoadSharingDiscoveryTask: Started background discovery");
}

void LoadSharingDiscoveryTask::end() {
  cleanupQuery();
  MicroTask.stopTask(this);
  DBUGF("LoadSharingDiscoveryTask: Stopped background discovery");
}

void LoadSharingDiscoveryTask::triggerDiscovery() {
  // Reset discovery timer to force immediate query on next task wake
  _last_discovery_time = 0;
  MicroTask.wakeTask(this);
  DBUGF("LoadSharingDiscoveryTask: Triggered manual discovery");
}

const std::vector<DiscoveredPeer>& LoadSharingDiscoveryTask::getCachedPeers() const {
  return _cachedPeers;
}

bool LoadSharingDiscoveryTask::isCacheValid() const {
  if (!_cacheValid) {
    return false;
  }

  return (millis() - _lastDiscovery < _cacheTtl);
}

void LoadSharingDiscoveryTask::invalidateCache() {
  _cacheValid = false;
  _cachedPeers.clear();
  DBUGLN("Discovery cache invalidated");
}

unsigned long LoadSharingDiscoveryTask::cacheTimeRemaining() const {
  if (!_cacheValid) {
    return 0;
  }

  unsigned long elapsed = millis() - _lastDiscovery;
  if (elapsed >= _cacheTtl) {
    return 0;
  }

  return _cacheTtl - elapsed;
}

void LoadSharingDiscoveryTask::startAsyncQuery() {
  // Start an async mDNS query for OpenEVSE services
  // Parameters:
  //   - name: NULL (search for all instances)
  //   - service_type: "openevse" (without leading underscore)
  //   - proto: "tcp" (without leading underscore)
  //   - type: MDNS_TYPE_PTR (PTR record for service discovery)
  //   - timeout_ms: query timeout
  //   - max_results: 20 (collect up to 20 results)
  //   - notifier: NULL (we'll poll instead)
  _active_query = (void*)mdns_query_async_new(
      NULL,
      "openevse",
      "tcp",
      MDNS_TYPE_PTR,
      _query_timeout_ms,
      20,
      NULL
  );

  if (_active_query) {
    _query_in_progress = true;
    _query_start_time = millis();
    DBUGLN("LoadSharingDiscoveryTask: Async query started");
  } else {
    DBUGLN("LoadSharingDiscoveryTask: ERROR - Failed to start async query");
    _query_in_progress = false;
  }
}

bool LoadSharingDiscoveryTask::pollAsyncQuery() {
  if (!_active_query) {
    return false;
  }

  mdns_result_t* results = nullptr;

  // Poll for results with short timeout to avoid blocking
  // Returns true when query is complete (whether or not results were found)
  bool isComplete = mdns_query_async_get_results(
      (mdns_search_once_t*)_active_query,
      100,  // 100ms polling timeout
      &results
  );

  if (isComplete) {
    unsigned long elapsed = millis() - _query_start_time;

    // Convert mdns_result_t linked list to our DiscoveredPeer vector
    std::vector<DiscoveredPeer> peers;
    std::vector<String> seenHostnames;  // Track to deduplicate

    for (mdns_result_t* r = results; r; r = r->next) {
      DiscoveredPeer peer;

      // Build hostname
      if (r->instance_name) {
        peer.serviceName = String(r->instance_name);
        peer.hostname = peer.serviceName + String(".local");
      } else if (r->hostname) {
        peer.hostname = String(r->hostname);
        if (!peer.hostname.endsWith(".local")) {
          peer.hostname += ".local";
        }
        peer.serviceName = r->hostname;
      } else {
        continue;  // Skip if no hostname
      }

      // Check for duplicates
      bool isDuplicate = false;
      for (const auto &seen : seenHostnames) {
        if (seen == peer.hostname) {
          isDuplicate = true;
          break;
        }
      }

      if (isDuplicate) {
        continue;
      }

      // Extract IP address
      if (r->addr) {
        uint32_t ip = r->addr->addr.u_addr.ip4.addr;
        peer.ipAddress = String((ip & 0xFF)) + "." +
                        String((ip >> 8) & 0xFF) + "." +
                        String((ip >> 16) & 0xFF) + "." +
                        String((ip >> 24) & 0xFF);
      }

      peer.port = r->port;
      peer.discoveredAt = millis();

      // Extract TXT records
      for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key && r->txt[i].value) {
          peer.txtRecords[String(r->txt[i].key)] = String(r->txt[i].value);
        }
      }

      DBUGF("  Found peer: %s (%s:%u)",
            peer.hostname.c_str(), peer.ipAddress.c_str(), peer.port);

      seenHostnames.push_back(peer.hostname);
      peers.push_back(peer);
    }

    DBUGF("LoadSharingDiscoveryTask: Query complete in %lu ms, found %u peers",
          elapsed, (unsigned int)peers.size());

    // Clean up mDNS results
    if (results) {
      mdns_query_results_free(results);
    }

    // Update our cache with the new results
    _cachedPeers = peers;
    _lastDiscovery = millis();
    _cacheValid = true;

    processQueryResults(peers);

    return true;  // Query is complete
  }

  return false;  // Query still in progress
}

void LoadSharingDiscoveryTask::processQueryResults(const std::vector<DiscoveredPeer>& peers) {
  // Update statistics
  _last_result_count = peers.size();

  DBUGF("LoadSharingDiscoveryTask: Processed %u peer discovery results", (unsigned int)peers.size());
}

void LoadSharingDiscoveryTask::cleanupQuery() {
  if (_active_query != nullptr) {
    mdns_query_async_delete((mdns_search_once_t*)_active_query);
    _active_query = nullptr;
  }
  _query_in_progress = false;
}

// Group peer management methods

bool LoadSharingDiscoveryTask::addGroupPeer(const String& hostname) {
  // Check for duplicates
  for (const auto& peer : _groupPeers) {
    if (peer == hostname) {
      DBUGF("LoadSharingDiscoveryTask: Peer already in group: %s", hostname.c_str());
      return false;
    }
  }

  _groupPeers.push_back(hostname);
  _groupPeersDirty = true;
  saveGroupPeers();

  DBUGF("LoadSharingDiscoveryTask: Added peer to group: %s (total: %u)",
        hostname.c_str(), _groupPeers.size());

  return true;
}

bool LoadSharingDiscoveryTask::removeGroupPeer(const String& hostname) {
  for (size_t i = 0; i < _groupPeers.size(); i++) {
    if (_groupPeers[i] == hostname) {
      _groupPeers.erase(_groupPeers.begin() + i);
      _groupPeersDirty = true;
      saveGroupPeers();

      DBUGF("LoadSharingDiscoveryTask: Removed peer from group: %s (remaining: %u)",
            hostname.c_str(), _groupPeers.size());

      return true;
    }
  }

  DBUGF("LoadSharingDiscoveryTask: Peer not found: %s", hostname.c_str());
  return false;
}

const std::vector<String>& LoadSharingDiscoveryTask::getGroupPeers() const {
  return _groupPeers;
}

bool LoadSharingDiscoveryTask::isGroupPeer(const String& hostname) const {
  for (const auto& peer : _groupPeers) {
    if (peer == hostname) {
      return true;
    }
  }
  return false;
}

std::vector<LoadSharingDiscoveryTask::PeerInfo> LoadSharingDiscoveryTask::getAllPeers(
    bool includeDiscovered, bool includeGroup) const {

  std::vector<PeerInfo> result;
  std::vector<String> addedHosts;

  // Add discovered peers
  if (includeDiscovered) {
    for (const auto& peer : _cachedPeers) {
      PeerInfo info;
      info.hostname = peer.hostname;
      info.ipAddress = peer.ipAddress;
      info.online = true;
      info.joined = isGroupPeer(peer.hostname);

      result.push_back(info);
      addedHosts.push_back(peer.hostname);
    }
  }

  // Add group peers that weren't discovered
  if (includeGroup) {
    for (const auto& hostname : _groupPeers) {
      // Check if already added
      bool alreadyAdded = false;
      for (const auto& added : addedHosts) {
        if (added == hostname) {
          alreadyAdded = true;
          break;
        }
      }

      if (!alreadyAdded) {
        PeerInfo info;
        info.hostname = hostname;
        info.ipAddress = "";
        info.online = false;
        info.joined = true;

        result.push_back(info);
      }
    }
  }

  return result;
}

bool LoadSharingDiscoveryTask::loadGroupPeers() {
  const char* filePath = "/loadsharing_peers.json";

  if (!LittleFS.exists(filePath)) {
    DBUGLN("LoadSharingDiscoveryTask: No persisted group peer list found, starting with empty list");
    return false;
  }

  File file = LittleFS.open(filePath, "r");
  if (!file) {
    DBUGF("LoadSharingDiscoveryTask: Failed to open group peer list file: %s", filePath);
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    DBUGF("LoadSharingDiscoveryTask: Failed to parse group peer list JSON: %s", error.c_str());
    return false;
  }

  _groupPeers.clear();
  JsonArray peers = doc["peers"].as<JsonArray>();
  for (JsonVariant peer : peers) {
    String hostname = peer.as<String>();
    _groupPeers.push_back(hostname);
  }

  _groupPeersDirty = false;
  DBUGF("LoadSharingDiscoveryTask: Loaded %u group peers", _groupPeers.size());

  return true;
}

bool LoadSharingDiscoveryTask::saveGroupPeers() {
  if (!_groupPeersDirty) {
    return true;  // No changes to save
  }

  const char* filePath = "/loadsharing_peers.json";
  const char* tempPath = "/loadsharing_peers.json.tmp";

  // Write to temp file first
  File file = LittleFS.open(tempPath, "w");
  if (!file) {
    DBUGF("LoadSharingDiscoveryTask: Failed to open temp file for writing: %s", tempPath);
    return false;
  }

  DynamicJsonDocument doc(1024);
  JsonArray peers = doc.createNestedArray("peers");
  for (const auto& hostname : _groupPeers) {
    peers.add(hostname);
  }

  if (serializeJson(doc, file) == 0) {
    file.close();
    DBUGLN("LoadSharingDiscoveryTask: Failed to write group peer list JSON");
    return false;
  }

  file.close();

  // Atomic rename (replace old with new)
  if (LittleFS.exists(filePath)) {
    LittleFS.remove(filePath);
  }
  if (!LittleFS.rename(tempPath, filePath)) {
    DBUGF("LoadSharingDiscoveryTask: Failed to rename temp file to %s", filePath);
    return false;
  }

  _groupPeersDirty = false;
  DBUGF("LoadSharingDiscoveryTask: Saved %u group peers", _groupPeers.size());

  return true;
}
