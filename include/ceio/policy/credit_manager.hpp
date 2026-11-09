#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "ceio/net/packet.hpp"

namespace ceio::policy {

/// CEIO credit bookkeeping ([CEIO] Sec. 4.1, Algorithm 1), independent of time.
///
/// One credit = one I/O buffer in the LLC. Credits move between three places:
/// a flow's balance `credits`, its `outstanding` count (consumed by fast-path
/// packets not yet released), and the `free` pool (only used while no flow is
/// active). Debts `owes` record credits a flow still has to pay to flows that
/// joined while it was short. The invariant
///     sum(credits) + sum(outstanding) + free == total
/// holds after every operation (checked by invariant_ok()).
///
/// Transcription choices (docs/spec.md Sec. 5.1): `min` instead of the printed
/// `max` in the release loop; j ranges over the m new flows; the uniform split
/// of line 12 is the default and the literal sequential decay is available via
/// `literal_split`. Integer credits: shares are floored, remainders go to the
/// lowest flow ids, and a debtor pays at least one credit per creditor per
/// release so small releases still settle debts.
class CreditManager {
 public:
  explicit CreditManager(std::int64_t total, bool literal_split = false);

  /// Algorithm 1, assignment: admits `flows` (new, or inactive being re-activated).
  void add_flows(const std::vector<net::FlowId>& flows);
  /// Marks a flow inactive; its positive balance is redistributed to active flows
  /// and debts to or from it are forgiven ([CEIO] Sec. 4.1 Q3).
  void deactivate(net::FlowId f);
  /// A fast-path packet of `f` takes `n` credits (balance may go negative when
  /// exhaustion is detected late).
  void consume(net::FlowId f, std::int64_t n = 1);
  /// Algorithm 1, release: `gamma` credits of `f` come back. Inactive flows'
  /// releases go to the active flows.
  void release(net::FlowId f, std::int64_t gamma);

  bool known(net::FlowId f) const { return flows_.count(f) != 0; }
  bool active(net::FlowId f) const;
  std::int64_t credits(net::FlowId f) const;
  std::int64_t outstanding(net::FlowId f) const;
  std::int64_t debt_of(net::FlowId f) const;   ///< total this flow still owes
  std::int64_t owed_to(net::FlowId f) const;   ///< total others still owe this flow
  std::size_t active_count() const;
  std::int64_t total() const { return total_; }
  std::int64_t free_pool() const { return free_; }
  bool invariant_ok() const;

 private:
  struct Entry {
    std::int64_t credits = 0;
    std::int64_t outstanding = 0;
    bool active = false;
    std::map<net::FlowId, std::int64_t> owes;  // creditor -> amount
  };
  /// Splits `amount` among active flows except `except` (or into `free_`).
  void distribute(std::int64_t amount, const net::FlowId* except);
  Entry& at(net::FlowId f);

  std::int64_t total_;
  bool literal_;
  std::int64_t free_;
  std::map<net::FlowId, Entry> flows_;  // ordered: deterministic iteration
};

}  // namespace ceio::policy
