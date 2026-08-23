#include "nimbus/provider_audit.h"

#include <ArduinoJson.h>

namespace core {

int parseMistralConnectors(const char* json, std::vector<AuditConnector>& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return 0;
  // Mistral returns {"items":[...],"pagination":{...}}; tolerate a bare array too.
  JsonArrayConst items = doc["items"].as<JsonArrayConst>();
  if (items.isNull()) items = doc.as<JsonArrayConst>();
  if (items.isNull()) return 0;
  int n = 0;
  for (JsonObjectConst c : items) {
    const char* name = c["name"] | "";
    const char* id = c["id"] | "";
    if (!name[0] && !id[0]) continue;
    AuditConnector a;
    a.name = name;
    a.id = id;
    a.protocol = c["protocol"] | "";
    out.push_back(a);
    n++;
  }
  return n;
}

int parseModelsList(const char* json, std::vector<std::string>& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return 0;
  JsonArrayConst data = doc["data"].as<JsonArrayConst>();
  if (data.isNull()) data = doc.as<JsonArrayConst>();  // tolerate a bare array
  if (data.isNull()) return 0;
  int n = 0;
  for (JsonObjectConst m : data) {
    const char* id = m["id"] | "";
    if (!id[0]) continue;
    out.push_back(id);
    n++;
  }
  return n;
}

std::string connectorIdByName(const std::vector<AuditConnector>& cs,
                              const std::string& name) {
  for (const AuditConnector& c : cs)
    if (c.name == name) return c.id;
  return std::string();
}

}  // namespace core
