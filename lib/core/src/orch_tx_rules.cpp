// The stub rule and its neighbours (tx/rules.h). Stub bodies from lane RB-B0a: each
// answers Err::NotImplemented until lane RB-T3 writes the rules.
#include "nimbus/orch/tx/rules.h"

namespace nimbus {
namespace orch {
namespace tx {

Status decideTurnOpen(const OpenFacts&, PointerMove&) { return Status::pending("tx::decideTurnOpen"); }
Status stubText(const StubFacts&, std::string&) { return Status::pending("tx::stubText"); }
Status longArgStub(uint32_t, const std::string&, uint64_t, std::string&) { return Status::pending("tx::longArgStub"); }
Status photoStub(const std::string&, const std::string&, std::string&) { return Status::pending("tx::photoStub"); }
Status supersededBy(const Item&, const std::vector<Item>&, std::vector<uint64_t>&) {
  return Status::pending("tx::supersededBy");
}
Status visiblePhotos(const PhotoFacts&, std::vector<std::string>&) { return Status::pending("tx::visiblePhotos"); }
Status nextRung(const LadderFacts&, LadderRung&) { return Status::pending("tx::nextRung"); }
Status omissionLine(uint32_t, const std::string&, std::string&) { return Status::pending("tx::omissionLine"); }

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
