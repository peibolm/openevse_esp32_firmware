/*
 * MIT License
 * Copyright (c) 2025 Jeremy Poulter
 *
 * Load Sharing Discovery - mDNS peer discovery wrapper
 * Queries for OpenEVSE peers on the local network and caches results
 */

#ifndef LOADSHARING_DISCOVERY_H
#define LOADSHARING_DISCOVERY_H

#include <Arduino.h>
#include <vector>
#include <map>

/**
 * @brief Discovered OpenEVSE peer information
 */
struct DiscoveredPeer {
  String hostname;      // Fully qualified hostname (e.g., "openevse-7856.local")
  String serviceName;   // Service instance name (e.g., "openevse-7856")
  String ipAddress;     // IP address as string
  uint16_t port;        // Service port
  std::map<String, String> txtRecords;  // TXT records (version, type, id, etc.)
  unsigned long discoveredAt;  // Timestamp when discovered (millis())
};

/**
 * @brief Load Sharing Discovery Manager
 * 
 * Provides mDNS-based discovery of OpenEVSE peers with caching.
 * Automatically refreshes cache based on TTL.
 */
class LoadSharingDiscovery {
public:
  /**
   * @brief Initialize discovery
   * @param cacheTtl Time-to-live for cached results in milliseconds (default: 60000ms)
   */
  void begin(unsigned long cacheTtl = 60000);
  
  /**
   * @brief Clean up resources
   */
  void end();
  
  /**
   * @brief Query for OpenEVSE peers on the network
   * 
   * Performs mDNS query for _openevse._tcp services and returns cached results.
   * Will refresh cache if TTL has expired.
   * 
   * @param timeout Query timeout in milliseconds (default: 2000ms)
   * @return Vector of discovered peers
   */
  std::vector<DiscoveredPeer> discoverPeers(unsigned long timeout = 2000);
  
  /**
   * @brief Get the currently cached peer list
   * 
   * @return Vector of cached peers (may be empty or stale)
   */
  const std::vector<DiscoveredPeer>& getCachedPeers() const;
  
  /**
   * @brief Check if cached results are still valid
   * 
   * @return true if cache is valid and within TTL
   */
  bool isCacheValid() const;
  
  /**
   * @brief Force cache refresh on next query
   */
  void invalidateCache();
  
  /**
   * @brief Get time remaining on cache TTL
   * 
   * @return Milliseconds remaining, or 0 if cache is expired
   */
  unsigned long cacheTimeRemaining() const;
  
private:
  std::vector<DiscoveredPeer> _cachedPeers;
  unsigned long _lastDiscovery;  // Timestamp of last successful discovery
  unsigned long _cacheTtl;       // Cache time-to-live in milliseconds
  bool _cacheValid;              // Whether cache has been populated
};

// Global discovery instance
extern LoadSharingDiscovery loadSharingDiscovery;

#endif // LOADSHARING_DISCOVERY_H
