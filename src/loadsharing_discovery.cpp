/*
 * MIT License
 * Copyright (c) 2025 Jeremy Poulter
 *
 * Load Sharing Discovery - mDNS peer discovery implementation
 */

#include "loadsharing_discovery.h"
#include <ESPmDNS.h>
#include "debug.h"

// Global discovery instance
LoadSharingDiscovery loadSharingDiscovery;

void LoadSharingDiscovery::begin(unsigned long cacheTtl) {
  _cacheTtl = cacheTtl;
  _lastDiscovery = 0;
  _cacheValid = false;
  _cachedPeers.clear();
  
  DBUGF("Load Sharing Discovery initialized with TTL: %lu ms\n", _cacheTtl);
}

void LoadSharingDiscovery::end() {
  _cachedPeers.clear();
  _cacheValid = false;
}

std::vector<DiscoveredPeer> LoadSharingDiscovery::discoverPeers(unsigned long timeout) {
  // Check if cache is still valid
  if (_cacheValid && (millis() - _lastDiscovery < _cacheTtl)) {
    DBUGLN("Using cached peer list");
    return _cachedPeers;
  }
  
  // Cache expired or not yet populated - perform fresh discovery
  DBUGLN("Querying mDNS for OpenEVSE peers (_openevse._tcp)");
  
  _cachedPeers.clear();
  
  // Query for OpenEVSE services
  int numServices = MDNS.queryService("openevse", "tcp");
  
  if (numServices > 0) {
    DBUGF("Found %d OpenEVSE service(s)\n", numServices);
    
    // Process each discovered service
    for (int i = 0; i < numServices; i++) {
      DiscoveredPeer peer;
      
      // Get basic service information
      peer.serviceName = MDNS.hostname(i);
      peer.hostname = peer.serviceName + String(".local");
      peer.ipAddress = MDNS.IP(i).toString();
      peer.port = MDNS.port(i);
      peer.discoveredAt = millis();
      
      // Extract TXT records
      int numTxt = MDNS.numTxt(i);
      for (int j = 0; j < numTxt; j++) {
        String key = MDNS.txtKey(i, j);
        String value = MDNS.txt(i, j);
        peer.txtRecords[key] = value;
      }
      
      DBUGF("  Peer[%d]: %s (%s:%u)\n", i, peer.hostname.c_str(), 
            peer.ipAddress.c_str(), peer.port);
      
      // Log TXT records
      for (auto& txt : peer.txtRecords) {
        DBUGF("    %s: %s\n", txt.first.c_str(), txt.second.c_str());
      }
      
      _cachedPeers.push_back(peer);
    }
  } else {
    DBUGLN("No OpenEVSE services found on network");
  }
  
  // Update cache metadata
  _lastDiscovery = millis();
  _cacheValid = true;
  
  DBUGF("Discovery complete. Cached %u peers\n", _cachedPeers.size());
  
  return _cachedPeers;
}

const std::vector<DiscoveredPeer>& LoadSharingDiscovery::getCachedPeers() const {
  return _cachedPeers;
}

bool LoadSharingDiscovery::isCacheValid() const {
  if (!_cacheValid) {
    return false;
  }
  
  return (millis() - _lastDiscovery < _cacheTtl);
}

void LoadSharingDiscovery::invalidateCache() {
  _cacheValid = false;
  _lastDiscovery = 0;
  DBUGLN("Peer cache invalidated");
}

unsigned long LoadSharingDiscovery::cacheTimeRemaining() const {
  if (!_cacheValid) {
    return 0;
  }
  
  unsigned long elapsed = millis() - _lastDiscovery;
  if (elapsed >= _cacheTtl) {
    return 0;
  }
  
  return _cacheTtl - elapsed;
}
