#pragma once
#include <cstdint>

// Ported verbatim from Nuage-Solide lib/core/telegram_offset.h.
//
// Telegram long-poll offset arithmetic, isolated here so it is host-testable (the
// rest of the telegram client pulls in WiFiClientSecure and can't build natively).
//
// getUpdates?offset=N returns every update with update_id >= N and CONFIRMS
// (deletes server-side) those with id < N. So after handling an update with id
// `updateId`, the next offset must be updateId + 1 - otherwise the same update is
// re-delivered on every poll forever (re-running the decider and flooding TLS).
namespace nimbus {
namespace core {

inline int32_t nextTelegramOffset(int32_t currentOffset, int32_t updateId) {
  int32_t next = updateId + 1;
  return next > currentOffset ? next : currentOffset;  // never move the offset backwards
}

}  // namespace core
}  // namespace nimbus
