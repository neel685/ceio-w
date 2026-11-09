// ceio_run: run one scenario from command-line options and print JSON results.
//
//   ceio_run --testbed=toasty --app=nat --rate=100 --pkt=512 --pool=16384 --ring=4096
//   ceio_run --testbed=ceio --app=kv --cores=8 --rate=200 --pkt=144 --bypass=2
//
// Run with --help for all options.

#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

#include "ceio/exp/experiment.hpp"

using namespace ceio;

namespace {

const char* kHelp = R"(ceio_run - simulate one I/O scenario and print JSON results

Hardware
  --testbed=ceio|toasty      hardware profile (default ceio)
  --pressure=GBPS            co-runner memory bandwidth in GB/s (default 0)
CPU-involved flows (one queue + one core per --cores)
  --app=NAME                 echo|kv|nat|ids|l2fwd (default echo)
  --cores=N                  queues/cores, one flow each (default 1)
  --rate=GBPS                aggregate offered load (default: line rate)
  --pkt=BYTES                packet size (default: 1024 ceio, 512 toasty)
  --pattern=P                constant|poisson|microburst|poissonrate (default constant)
  --burst-pkts=N             microburst size (default 2048)
  --burst-period-us=US       microburst period (default 1000)
  --pool=N                   buffers per queue (default 16384)
  --ring=N                   descriptor ring size (default 4096)
  --buf=BYTES                buffer size (default: 2048 ceio, 4096 toasty)
  --order=fifo|lifo          free-buffer order (default fifo)
CPU-bypass (RDMA write) flows
  --bypass=N                 number of RDMA flows/queues (default 0)
  --bypass-rate=GBPS         per-flow rate (default 20)
  --bypass-msg=BYTES         message size (default 65536)
I/O policy
  --policy=baseline|ceio     receive-path scheme (default baseline)
  --ceio-credits=N           total credits (default: DDIO bytes / buffer size)
  --ceio-recv=sync|async     slow-path fetch API (default async)
  --ceio-ideal=0|1           per-packet credit check instead of counter polling
Run control
  --warmup-ms=MS             (default 2)    --measure-ms=MS  (default 4)
  --seed=N                   (default 1)
  --saturate=LOSS            instead of running at --rate, search for the highest
                             CPU-involved load with loss <= LOSS (e.g. 0.001) and
                             report the run at that load (adds "offered_gbps")
)";

std::map<std::string, std::string> parse(int argc, char** argv) {
  std::map<std::string, std::string> m;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::cout << kHelp;
      std::exit(0);
    }
    if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument '" + a + "'");
    const auto eq = a.find('=');
    if (eq == std::string::npos) throw std::invalid_argument("expected --key=value, got '" + a + "'");
    m[a.substr(2, eq - 2)] = a.substr(eq + 1);
  }
  return m;
}

struct Opts {
  std::map<std::string, std::string> m;
  std::string str(const std::string& k, const std::string& d) {
    auto it = m.find(k);
    if (it == m.end()) return d;
    std::string v = it->second;
    m.erase(it);
    return v;
  }
  double num(const std::string& k, double d) {
    const std::string v = str(k, "");
    return v.empty() ? d : std::stod(v);
  }
};

net::TrafficSpec::Pattern pattern_of(const std::string& p) {
  if (p == "constant") return net::TrafficSpec::Pattern::kConstant;
  if (p == "poisson") return net::TrafficSpec::Pattern::kPoisson;
  if (p == "microburst") return net::TrafficSpec::Pattern::kMicroburst;
  if (p == "poissonrate") return net::TrafficSpec::Pattern::kPoissonRate;
  throw std::invalid_argument("unknown pattern '" + p + "'");
}

exp::Scenario build(Opts& o) {
  const std::string tb = o.str("testbed", "ceio");
  exp::Scenario s;
  bool toasty = false;
  if (tb == "ceio") {
    s = exp::ceio_testbed();
  } else if (tb == "toasty") {
    s = exp::toasty_testbed();
    toasty = true;
  } else {
    throw std::invalid_argument("unknown testbed '" + tb + "'");
  }
  s.seed = static_cast<std::uint64_t>(o.num("seed", 1));
  s.warmup = from_us(o.num("warmup-ms", 2) * 1000);
  s.measure = from_us(o.num("measure-ms", 4) * 1000);
  s.pressure.gbytes_per_s = o.num("pressure", 0);

  const auto cores = static_cast<std::uint32_t>(o.num("cores", 1));
  const double rate = o.num("rate", s.link.rate_gbps);
  const auto pkt = static_cast<std::uint32_t>(o.num("pkt", toasty ? 512 : 1024));

  exp::QueueSetup q;
  q.app = host::AppProfile::by_name(o.str("app", "echo"));
  q.pool_buffers = static_cast<std::uint32_t>(o.num("pool", 16384));
  q.ring_size = static_cast<std::uint32_t>(o.num("ring", 4096));
  q.buffer_bytes = static_cast<std::uint32_t>(o.num("buf", toasty ? 4096 : 2048));
  const std::string order = o.str("order", "fifo");
  if (order != "fifo" && order != "lifo") throw std::invalid_argument("--order must be fifo or lifo");
  q.order = order == "lifo" ? host::PoolOrder::kLifo : host::PoolOrder::kFifo;
  q.core.ghz = toasty ? 2.0 : 2.8;

  net::TrafficSpec t;
  t.pattern = pattern_of(o.str("pattern", "constant"));
  t.rate_gbps = rate / cores;
  t.burst_packets = static_cast<std::uint32_t>(o.num("burst-pkts", 2048));
  t.burst_period = from_us(o.num("burst-period-us", 1000));

  net::FlowId id = 1;
  for (std::uint32_t i = 0; i < cores; ++i) {
    s.queues.push_back(q);
    exp::FlowSetup f;
    f.spec.id = id++;
    f.spec.packet_bytes = pkt;
    f.spec.message_bytes = pkt;
    f.spec.traffic = t;
    f.queue = i;
    s.flows.push_back(f);
  }

  const auto bypass = static_cast<std::uint32_t>(o.num("bypass", 0));
  const double bypass_rate = o.num("bypass-rate", 20);
  const auto bypass_msg = static_cast<std::uint32_t>(o.num("bypass-msg", 65536));
  for (std::uint32_t i = 0; i < bypass; ++i) {
    exp::QueueSetup b;
    b.kind = nic::QueueKind::kRdma;
    b.app = host::AppProfile::rdma_sink();
    b.core.ghz = q.core.ghz;
    s.queues.push_back(b);
    exp::FlowSetup f;
    f.spec.id = id++;
    f.spec.packet_bytes = 1024;
    f.spec.message_bytes = bypass_msg;
    f.spec.traffic.rate_gbps = bypass_rate;
    f.queue = static_cast<std::uint32_t>(s.queues.size() - 1);
    s.flows.push_back(f);
  }
  const std::string policy = o.str("policy", "baseline");
  if (policy == "ceio") {
    s.policy = exp::IoPolicy::kCeio;
  } else if (policy != "baseline") {
    throw std::invalid_argument("unknown policy '" + policy + "'");
  }
  s.ceio.total_credits = static_cast<std::int64_t>(o.num("ceio-credits", 0));
  const std::string recv = o.str("ceio-recv", "async");
  if (recv != "sync" && recv != "async") throw std::invalid_argument("--ceio-recv must be sync or async");
  s.ceio.recv = recv == "sync" ? policy::CeioConfig::Recv::kSync : policy::CeioConfig::Recv::kAsync;
  s.ceio.ideal_detection = o.num("ceio-ideal", 0) != 0;
  s.name = tb + "/" + q.app.name + "/" + policy;
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Opts o{parse(argc, argv)};
    exp::Scenario s = build(o);
    const double saturate = o.num("saturate", -1);
    if (!o.m.empty()) throw std::invalid_argument("unknown option --" + o.m.begin()->first);
    if (saturate >= 0) {
      const exp::SaturationResult sat = exp::find_saturation(s, saturate);
      std::string j = exp::to_json(sat.at_rate);
      j.insert(1, "\"offered_gbps\":" + std::to_string(sat.offered_gbps) + ",\"search_runs\":" +
                      std::to_string(sat.runs) + ",");
      std::cout << j << "\n";
    } else {
      std::cout << exp::to_json(exp::run(s)) << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ceio_run: " << e.what() << "\n(see --help)\n";
    return 2;
  }
}
