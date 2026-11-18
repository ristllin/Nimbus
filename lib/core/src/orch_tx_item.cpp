// The transcript item codec (tx/item.h). Stub bodies from lane RB-B0a: each answers
// Err::NotImplemented until lane RB-T1 writes the codec.
#include "nimbus/orch/tx/item.h"

namespace nimbus {
namespace orch {
namespace tx {

Status encodeLine(const Item&, std::string&) { return Status::pending("tx::encodeLine"); }
Status decodeLine(const char*, size_t, Item&) { return Status::pending("tx::decodeLine"); }

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
