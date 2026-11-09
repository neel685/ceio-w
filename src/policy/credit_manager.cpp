#include "ceio/policy/credit_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ceio::policy {

CreditManager::CreditManager(std::int64_t total, bool literal_split)
    : total_(total), literal_(literal_split), free_(total) {
  if (total <= 0) throw std::invalid_argument("CreditManager: total credits must be > 0");
}

CreditManager::Entry& CreditManager::at(net::FlowId f) {
  auto it = flows_.find(f);
  if (it == flows_.end()) throw std::out_of_range("CreditManager: unknown flow " + std::to_string(f));
  return it->second;
}

bool CreditManager::active(net::FlowId f) const {
  auto it = flows_.find(f);
  return it != flows_.end() && it->second.active;
}

std::int64_t CreditManager::credits(net::FlowId f) const {
  auto it = flows_.find(f);
  return it == flows_.end() ? 0 : it->second.credits;
}

std::int64_t CreditManager::outstanding(net::FlowId f) const {
  auto it = flows_.find(f);
  return it == flows_.end() ? 0 : it->second.outstanding;
}

std::int64_t CreditManager::debt_of(net::FlowId f) const {
  auto it = flows_.find(f);
  if (it == flows_.end()) return 0;
  std::int64_t d = 0;
  for (const auto& [j, o] : it->second.owes) d += o;
  return d;
}

std::int64_t CreditManager::owed_to(net::FlowId f) const {
  std::int64_t d = 0;
  for (const auto& [i, e] : flows_) {
    auto it = e.owes.find(f);
    if (it != e.owes.end()) d += it->second;
  }
  return d;
}

std::size_t CreditManager::active_count() const {
  std::size_t n = 0;
  for (const auto& [f, e] : flows_) n += e.active;
  return n;
}

bool CreditManager::invariant_ok() const {
  std::int64_t sum = free_;
  for (const auto& [f, e] : flows_) sum += e.credits + e.outstanding;
  return sum == total_;
}

void CreditManager::distribute(std::int64_t amount, const net::FlowId* except) {
  if (amount <= 0) return;
  std::vector<Entry*> targets;
  for (auto& [f, e] : flows_)
    if (e.active && (!except || f != *except)) targets.push_back(&e);
  if (targets.empty()) {
    free_ += amount;
    return;
  }
  const auto n = static_cast<std::int64_t>(targets.size());
  const std::int64_t each = amount / n;
  std::int64_t rem = amount % n;
  for (Entry* e : targets) {
    e->credits += each + (rem > 0 ? 1 : 0);
    if (rem > 0) --rem;
  }
}

void CreditManager::add_flows(const std::vector<net::FlowId>& news) {
  if (news.empty()) return;
  std::vector<net::FlowId> existing;
  for (const auto& [f, e] : flows_)
    if (e.active && std::find(news.begin(), news.end(), f) == news.end()) existing.push_back(f);
  for (net::FlowId j : news) {
    Entry& e = flows_[j];  // keeps outstanding/credits of a re-activated flow
    if (e.active) throw std::logic_error("CreditManager: flow " + std::to_string(j) + " already active");
    e.active = true;
  }
  // Credits parked in the free pool (no active flows earlier) go to the newcomers.
  if (free_ > 0) {
    const std::int64_t pool = free_;
    free_ = 0;
    const auto m = static_cast<std::int64_t>(news.size());
    std::int64_t rem = pool % m;
    for (net::FlowId j : news) {
      flows_[j].credits += pool / m + (rem > 0 ? 1 : 0);
      if (rem > 0) --rem;
    }
  }
  if (existing.empty()) return;

  const auto n = static_cast<std::int64_t>(existing.size());
  const auto m = static_cast<std::int64_t>(news.size());
  const std::int64_t c_flow = total_ / (n + m);   // line 2
  const std::int64_t share = c_flow / n;          // (1/n) C_flow per (i, j) pair
  for (net::FlowId i : existing) {
    Entry& ei = flows_[i];
    if (ei.credits >= m * share) {  // lines 4-6
      for (net::FlowId j : news) {
        flows_[j].credits += share;
        ei.credits -= share;
      }
      continue;
    }
    // Lines 8-14: i is short; split what it has and record the shortfall as debt.
    const std::int64_t snapshot = std::max<std::int64_t>(ei.credits, 0);
    for (net::FlowId j : news) {
      const std::int64_t give = literal_ ? std::max<std::int64_t>(ei.credits, 0) / m : snapshot / m;
      flows_[j].credits += give;
      ei.credits -= give;
      const std::int64_t debt = share - give;
      if (debt > 0) ei.owes[j] += debt;
    }
  }
}

void CreditManager::deactivate(net::FlowId f) {
  Entry& e = at(f);
  if (!e.active) return;
  e.active = false;
  e.owes.clear();
  for (auto& [i, other] : flows_) other.owes.erase(f);
  const std::int64_t reclaim = std::max<std::int64_t>(e.credits, 0);
  e.credits -= reclaim;
  distribute(reclaim, &f);
}

void CreditManager::consume(net::FlowId f, std::int64_t n) {
  Entry& e = at(f);
  e.credits -= n;
  e.outstanding += n;
}

void CreditManager::release(net::FlowId f, std::int64_t gamma) {
  if (gamma <= 0) return;
  Entry& e = at(f);
  e.outstanding -= gamma;
  if (!e.active) {
    distribute(gamma, &f);
    return;
  }
  if (!e.owes.empty()) {  // lines 19-25
    const auto creditors = static_cast<std::int64_t>(e.owes.size());
    const std::int64_t per = std::max<std::int64_t>(1, gamma / creditors);
    for (auto it = e.owes.begin(); it != e.owes.end() && gamma > 0;) {
      const std::int64_t pay = std::min({it->second, per, gamma});
      auto cj = flows_.find(it->first);
      if (cj != flows_.end() && cj->second.active) {
        cj->second.credits += pay;
      } else {
        e.credits += pay;  // creditor gone: keep it
      }
      it->second -= pay;
      gamma -= pay;
      it = it->second == 0 ? e.owes.erase(it) : std::next(it);
    }
  }
  e.credits += gamma;  // lines 18 / 23
}

}  // namespace ceio::policy
