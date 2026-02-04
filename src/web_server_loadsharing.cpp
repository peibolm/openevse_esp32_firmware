#if defined(ENABLE_DEBUG) && !defined(ENABLE_DEBUG_WEB)
#undef ENABLE_DEBUG
#endif

#include <Arduino.h>
#include "web_server.h"
#include "app_config.h"
#include "loadsharing_discovery_task.h"
#include "debug.h"
#include <vector>

typedef const __FlashStringHelper *fstr_t;

// Path prefix constants
#define LOADSHARING_PEERS_PATH "/loadsharing/peers"
#define LOADSHARING_PEERS_PATH_LEN (sizeof(LOADSHARING_PEERS_PATH) - 1)

// In-memory list of configured peer hosts
static std::vector<String> configuredPeers;

// Forward declarations
void handleLoadSharingPeersGet(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response);
void handleLoadSharingPeersPost(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response);
void handleLoadSharingPeersDelete(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response);
void handleLoadSharingPeersDeleteWithHost(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response, const String &host);
void handleLoadSharingDiscover(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response);

// -------------------------------------------------------------------
//
// url: /loadsharing/peers
// GET - list discovered and configured peers
// POST - add a new peer
// DELETE - remove a peer
//
// -------------------------------------------------------------------

void handleLoadSharingPeers(MongooseHttpServerRequest *request)
{
  MongooseHttpServerResponseStream *response;
  if(false == requestPreProcess(request, response)) {
    return;
  }

  if(HTTP_GET == request->method()) {
    handleLoadSharingPeersGet(request, response);
  } else if(HTTP_POST == request->method()) {
    handleLoadSharingPeersPost(request, response);
  } else if(HTTP_DELETE == request->method()) {
    handleLoadSharingPeersDelete(request, response);
  } else {
    response->setCode(405);
    response->print("{\"msg\":\"Method not allowed\"}");
  }

  request->send(response);
}

void handleLoadSharingPeersGet(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response)
{
  DBUGLN("[LoadSharing] GET /loadsharing/peers");
  
  // Build JSON response array (direct array, no wrapper object)
  const size_t capacity = JSON_ARRAY_SIZE(20) + JSON_OBJECT_SIZE(6) * 20 + 1024;
  DynamicJsonDocument doc(capacity);
  JsonArray peerArray = doc.to<JsonArray>();

  // Get the last discovered peers from the background discovery task (cached results)
  std::vector<DiscoveredPeer> discoveredPeers;
  // Use the task's cached results instead of doing a live query
  // This keeps the HTTP handler non-blocking
  discoveredPeers = loadSharingDiscoveryTask.getCachedPeers();
  DBUGF("[LoadSharing] Using cached results from background task. Query in progress: %s",
        loadSharingDiscoveryTask.isQueryInProgress() ? "yes" : "no");

  DBUGF("[LoadSharing] Found %d discovered peers", discoveredPeers.size());
  DBUGF("[LoadSharing] Found %d configured peers", configuredPeers.size());

  // Track which hosts we've already added (to avoid duplicates)
  std::vector<String> addedHosts;

  // Helper function to check if a host is in the configured group
  auto isJoined = [&configuredPeers](const String &host) -> bool {
    for(const auto &configuredHost : configuredPeers) {
      if (configuredHost == host) {
        return true;
      }
    }
    return false;
  };

  // Add discovered peers to response
  for(const auto &peer : discoveredPeers) {
    bool joined = isJoined(peer.hostname);
    DBUGF("[LoadSharing] Adding discovered peer: %s (joined: %s)", 
          peer.hostname.c_str(), joined ? "yes" : "no");
    
    JsonObject peerObj = peerArray.createNestedObject();
    peerObj["id"] = "unknown";
    peerObj["name"] = peer.hostname;
    peerObj["host"] = peer.hostname;
    peerObj["ip"] = peer.ipAddress;
    peerObj["online"] = true;
    peerObj["joined"] = joined;
    
    addedHosts.push_back(peer.hostname);
  }

  // Add configured peers that aren't already discovered (offline peers)
  for(const auto &configuredHost : configuredPeers) {
    // Check if this host is already in the response (discovered)
    bool alreadyAdded = false;
    for(const auto &addedHost : addedHosts) {
      if (addedHost == configuredHost) {
        alreadyAdded = true;
        break;
      }
    }
    
    if (!alreadyAdded) {
      DBUGF("[LoadSharing] Adding configured peer (offline): %s", configuredHost.c_str());
      
      JsonObject peerObj = peerArray.createNestedObject();
      peerObj["id"] = "unknown";
      peerObj["name"] = configuredHost;
      peerObj["host"] = configuredHost;
      peerObj["ip"] = "";
      peerObj["online"] = false;  // Not currently discovered
      peerObj["joined"] = true;   // Always true for configured peers
      
      addedHosts.push_back(configuredHost);
    }
  }

  response->setCode(200);
  serializeJson(doc, *response);
}

void handleLoadSharingPeersPost(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response)
{
  DBUGLN("[LoadSharing] POST /loadsharing/peers");
  
  // Parse request body
  String body = request->body().toString();
  DBUGF("[LoadSharing] Request body: %s", body.c_str());
  
  const size_t capacity = JSON_OBJECT_SIZE(2) + 256;
  DynamicJsonDocument doc(capacity);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    DBUGF("[LoadSharing] JSON parse error: %s", error.c_str());
    response->setCode(400);
    response->print("{\"msg\":\"Invalid JSON\"}");
    return;
  }
  
  // Get the host parameter
  if (!doc.containsKey("host")) {
    DBUGLN("[LoadSharing] Missing 'host' parameter");
    response->setCode(400);
    response->print("{\"msg\":\"Missing required 'host' parameter\"}");
    return;
  }
  
  String host = doc["host"].as<String>();
  host.trim();
  
  if (host.isEmpty()) {
    DBUGLN("[LoadSharing] Empty host parameter");
    response->setCode(400);
    response->print("{\"msg\":\"Host cannot be empty\"}");
    return;
  }
  
  DBUGF("[LoadSharing] Adding peer: %s", host.c_str());
  
  // Check for duplicates
  for (const auto &peer : configuredPeers) {
    if (peer == host) {
      DBUGF("[LoadSharing] Peer already configured: %s", host.c_str());
      response->setCode(400);
      response->print("{\"msg\":\"Peer already configured\"}");
      return;
    }
  }
  
  // Validate host is resolvable (basic check)
  if (host.indexOf('.') == -1 && host.indexOf(':') == -1) {
    DBUGF("[LoadSharing] Invalid host format: %s", host.c_str());
    response->setCode(400);
    response->print("{\"msg\":\"Invalid host format - must contain domain or IP\"}");
    return;
  }
  
  // Add to configured peers list
  configuredPeers.push_back(host);
  DBUGF("[LoadSharing] Peer added successfully. Total configured: %d", configuredPeers.size());
  
  response->setCode(200);
  response->print("{\"msg\":\"done\"}");
}

void handleLoadSharingPeersDelete(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response)
{
  // TODO: Phase 1.4 - Remove peer from configured group
  response->setCode(501);
  response->print("{\"msg\":\"Not implemented\"}");
}

void handleLoadSharingPeersDeleteWithHost(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response, const String &host)
{
  DBUGF("[LoadSharing] DELETE /loadsharing/peers/%s", host.c_str());
  
  // Find and remove the peer from configured list
  bool found = false;
  for(size_t i = 0; i < configuredPeers.size(); i++) {
    if(configuredPeers[i] == host) {
      DBUGF("[LoadSharing] Removing peer: %s", host.c_str());
      configuredPeers.erase(configuredPeers.begin() + i);
      found = true;
      break;
    }
  }
  
  if(!found) {
    DBUGF("[LoadSharing] Peer not found: %s", host.c_str());
    response->setCode(404);
    response->print("{\"msg\":\"Peer not found\"}");
    return;
  }
  
  DBUGF("[LoadSharing] Peer removed. Remaining peers: %d", configuredPeers.size());
  
  // Return success with empty data array or simplified response
  response->setCode(200);
  response->print("{\"msg\":\"done\"}");
}

void handleLoadSharingDiscover(MongooseHttpServerRequest *request, MongooseHttpServerResponseStream *response)
{
  DBUGLN("[LoadSharing] POST /loadsharing/discover");
  
  // Trigger manual discovery via the background task
  loadSharingDiscoveryTask.triggerDiscovery();
  DBUGLN("[LoadSharing] Triggered background discovery task");
  
  response->setCode(200);
  response->print("{\"msg\":\"done\"}");
}

void web_server_load_sharing_setup()
{
  // Register the /loadsharing/peers endpoint (GET, POST, DELETE)
  server.on("/loadsharing/peers", handleLoadSharingPeers);
  
  // Register the /loadsharing/peers/{host} endpoint (DELETE with path parameter)
  server.on("/loadsharing/peers/", [](MongooseHttpServerRequest *request) {
    MongooseHttpServerResponseStream *response;
    if(false == requestPreProcess(request, response)) {
      return;
    }

    // Extract the host parameter from the path
    String path = request->uri();
    if(path.length() > LOADSHARING_PEERS_PATH_LEN + 1) {
      String host = path.substring(LOADSHARING_PEERS_PATH_LEN + 1);
      // URL decode the host if needed
      DBUGF("[LoadSharing] DELETE path parameter: %s", host.c_str());
      
      if(HTTP_DELETE == request->method()) {
        handleLoadSharingPeersDeleteWithHost(request, response, host);
      } else {
        response->setCode(405);
        response->print("{\"msg\":\"Method not allowed\"}");
      }
    } else {
      response->setCode(400);
      response->print("{\"msg\":\"Invalid path\"}");
    }

    request->send(response);
  });

  // Register the /loadsharing/discover endpoint (POST for on-demand discovery)
  server.on("/loadsharing/discover", [](MongooseHttpServerRequest *request) {
    MongooseHttpServerResponseStream *response;
    if(false == requestPreProcess(request, response)) {
      return;
    }

    if(HTTP_POST == request->method()) {
      handleLoadSharingDiscover(request, response);
    } else {
      response->setCode(405);
      response->print("{\"msg\":\"Method not allowed\"}");
    }

    request->send(response);
  });
}
