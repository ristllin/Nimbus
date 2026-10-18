#include "sys/nvs_inventory.h"

#include <cstring>
#include <nvs.h>

namespace nimbus::sys {

namespace {

nimbus::store::NvsKind kindOf(nvs_type_t t) {
  switch (t) {
    case NVS_TYPE_U8: case NVS_TYPE_I8: case NVS_TYPE_U16: case NVS_TYPE_I16:
    case NVS_TYPE_U32: case NVS_TYPE_I32: case NVS_TYPE_U64: case NVS_TYPE_I64:
      return nimbus::store::NvsKind::Int;
    case NVS_TYPE_STR:  return nimbus::store::NvsKind::Str;
    case NVS_TYPE_BLOB: return nimbus::store::NvsKind::Blob;
    default:            return nimbus::store::NvsKind::Other;
  }
}

// The value's size only: a NULL out buffer makes NVS report the length without
// copying anything, so no value is ever read into this module.
uint32_t sizeOf(nvs_handle_t h, const char* key, nimbus::store::NvsKind kind) {
  size_t len = 0;
  if (kind == nimbus::store::NvsKind::Str && nvs_get_str(h, key, nullptr, &len) == ESP_OK)
    return (uint32_t)len;
  if (kind == nimbus::store::NvsKind::Blob && nvs_get_blob(h, key, nullptr, &len) == ESP_OK)
    return (uint32_t)len;
  return 0;
}

// One read-only handle per namespace, reused while consecutive keys share it.
struct NsHandle {
  char         ns[NVS_NS_NAME_MAX_SIZE] = {};
  nvs_handle_t h = 0;
  bool         open = false;

  bool use(const char* name) {
    if (open && strncmp(ns, name, sizeof(ns)) == 0) return true;
    close();
    strncpy(ns, name, sizeof(ns) - 1);
    open = nvs_open(name, NVS_READONLY, &h) == ESP_OK;
    return open;
  }
  void close() {
    if (open) nvs_close(h);
    open = false;
  }
};

}  // namespace

bool nvsEntryTotals(NvsEntryTotals& out) {
  out = NvsEntryTotals{};
  nvs_stats_t st = {};
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return false;
  out.used = (uint32_t)st.used_entries;
  out.free = (uint32_t)st.available_entries;
  out.total = (uint32_t)st.total_entries;
  out.namespaces = (uint32_t)st.namespace_count;
  return true;
}

size_t nvsForEachKey(const std::function<void(const NvsItem&)>& fn, size_t maxKeys,
                     bool* truncated) {
  if (truncated) *truncated = false;
  nvs_iterator_t it = nullptr;
  esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
  NsHandle ns;
  size_t listed = 0;
  while (e == ESP_OK) {
    if (listed >= maxKeys) {
      if (truncated) *truncated = true;
      break;
    }
    nvs_entry_info_t info = {};
    nvs_entry_info(it, &info);
    const nimbus::store::NvsKind kind = kindOf(info.type);
    const uint32_t bytes = ns.use(info.namespace_name) ? sizeOf(ns.h, info.key, kind) : 0;
    fn(NvsItem{info.namespace_name, info.key, kind, bytes,
               (uint32_t)nimbus::store::nvsEntriesFor(kind, bytes)});
    ++listed;
    e = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  ns.close();
  return listed;
}

}  // namespace nimbus::sys
