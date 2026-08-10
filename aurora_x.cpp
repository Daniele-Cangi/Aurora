// aurora_x.cpp - UPDATED VERSION (Nexus patch)
// AURORA-X - Extreme Field Orchestrator (3-file repo)
// Build Dev:   g++ -std=c++20 -O3 -pthread aurora_x.cpp -o aurora_x
// Build Field: g++ -std=c++20 -O3 -pthread -DFIELD_BUILD -DAURORA_USE_REAL_CRYPTO -DAURORA_USE_RAPTORQ_REAL aurora_x.cpp -lsodium -o aurora_x_field

#include <iostream>
#include <vector>
#include <string>
#include <array>
#include <memory>
#include <unordered_set>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <stdexcept>
using namespace std; using namespace chrono;

#include "aurora_intention.hpp"
#include "aurora_hal.hpp"
#include "aurora_extreme.hpp"
#include "aurora_organism.hpp"
#include "include/aurora/safety/SafetyMonitor.hpp"
#include "include/aurora/safety/SafetyEnvelope.hpp"
#include "include/aurora/telemetry/DecisionReplayLog.hpp"
#include "include/aurora/telemetry/SimulationEventLedger.hpp"
#include "include/aurora/simulation/GenerationArrivalSchedule.hpp"
#include "include/aurora/simulation/GenerationScheduler.hpp"
#ifdef AURORA_USE_LIBRAPTORQ
#include "fec/AuroraRaptorQ.hpp"
#endif

// Conversione enum Mode -> stringa (dopo include aurora_extreme.hpp)
static std::string mode_to_string(phy::Mode m) {
  switch(m) {
    case phy::Mode::RF: return "rf";
    case phy::Mode::IR: return "optical";
    case phy::Mode::BACKSCATTER: return "backscatter";
    default: return "unknown";
  }
}

static phy::Mode phy_link(aurora::safety::LinkMode mode) {
  switch (mode) {
    case aurora::safety::LinkMode::RF: return phy::Mode::RF;
    case aurora::safety::LinkMode::OPTICAL: return phy::Mode::IR;
    case aurora::safety::LinkMode::BACKSCATTER: return phy::Mode::BACKSCATTER;
  }
  return phy::Mode::RF;
}

// T1: Helper functions per convertire enum a stringhe
static std::string flowclass_to_string(aurora::FlowClass cls) {
  switch(cls) {
    case aurora::FlowClass::NERVE: return "NERVE";
    case aurora::FlowClass::GLAND: return "GLAND";
    case aurora::FlowClass::MUSCLE: return "MUSCLE";
    default: return "UNKNOWN";
  }
}

static std::string safetystate_to_string(aurora::safety::SafetyState state) {
  switch(state) {
    case aurora::safety::SafetyState::NO_EVIDENCE: return "NO_EVIDENCE";
    case aurora::safety::SafetyState::HEALTHY: return "HEALTHY";
    case aurora::safety::SafetyState::DEGRADED: return "DEGRADED";
    case aurora::safety::SafetyState::CRITICAL: return "CRITICAL";
    default: return "UNKNOWN";
  }
}

static std::string mode_to_string_cl(cl::Mode mode) {
  switch(mode) {
    case cl::Mode::CONSERVATIVE: return "CONSERVATIVE";
    case cl::Mode::NORMAL: return "NORMAL";
    case cl::Mode::AGGRESSIVE: return "AGGRESSIVE";
    default: return "UNKNOWN";
  }
}

// FASE 4: Usa aurora::safety::TelemetrySample invece di duplicare
// typedef aurora::safety::TelemetrySample TelemetrySample;
// Per ora estendiamo la struct locale per compatibilita
struct TelemetrySample {
  int step = 0;
  int have = 0;
  int need = 0;
  std::string mode;
  int tries = 0;
  int successes = 0;
  double reward = 0.0;
  double snr_rf = 0.0;
  double snr_ir = 0.0;
  double snr_bs = 0.0;
  double soc_src = 0.0;
  double duty_left = 0.0;
  double elapsed_s = 0.0;
  
  // FASE 4: FlowHealth metrics (allineati con aurora::safety::TelemetrySample)
  double nerve_fail_rate = 0.0;
  double gland_fail_rate = 0.0;
  double muscle_fail_rate = 0.0;
  double nerve_cov = 0.0;
  double gland_cov = 0.0;
  double muscle_cov = 0.0;
  int nerve_bad_streak = 0;
  int gland_bad_streak = 0;
  int muscle_bad_streak = 0;
  bool nerve_has_evidence = false;
  bool gland_has_evidence = false;
  bool muscle_has_evidence = false;
};

class TelemetrySink {
public:
  TelemetrySink() {
    const char* env = std::getenv("AURORA_TELEMETRY_PATH");
    if(env && *env) {
      _path = env;
    } else {
      _path = "aurora_telemetry.jsonl";
    }
    _samples.reserve(512);
  }

  void record(const TelemetrySample& s) {
    _samples.push_back(s);
  }

  void flush() {
    if(_samples.empty()) return;
    std::ofstream out(_path, std::ios::app);
    if(!out) {
      std::cerr << "[WARN] Telemetry non scrivibile su " << _path << "\n";
      return;
    }
    out << std::fixed << std::setprecision(3);
    for(const auto& s : _samples) {
      out << "{"
          << "\"step\":" << s.step << ","
          << "\"have\":" << s.have << ","
          << "\"need\":" << s.need << ","
          << "\"mode\":\"" << s.mode << "\","
          << "\"tries\":" << s.tries << ","
          << "\"successes\":" << s.successes << ","
          << "\"reward\":" << s.reward << ","
          << "\"snr_rf\":" << s.snr_rf << ","
          << "\"snr_ir\":" << s.snr_ir << ","
          << "\"snr_bs\":" << s.snr_bs << ","
          << "\"soc_src\":" << s.soc_src << ","
          << "\"duty_left\":" << s.duty_left << ","
          << "\"elapsed_s\":" << s.elapsed_s
          << "}\n";
    }
    std::cout << "[TELEMETRY] wrote " << _samples.size() << " samples -> " << _path << "\n";
    _samples.clear();
  }

private:
  std::string _path;
  std::vector<TelemetrySample> _samples;
};

// ---------- Token/Bundle ----------
struct Token {
  string id, payload; uint64_t created=0, expiry=0;
  array<uint8_t,32> pk{}; array<uint8_t,64> sk{}; array<uint8_t,64> sig{};
  static Token make(const string& payload, uint64_t ttl, uint64_t created_at_s=util::now_s()){
    Token t; CRYPTO::ed25519_keypair(t.pk.data(), t.sk.data());
    t.id = util::h64(payload + to_string(ttl) + to_string(util::rng.next()));
    t.payload = payload; t.created = created_at_s; t.expiry = t.created + ttl;
    string msg = t.id + t.payload + to_string(t.expiry);
    CRYPTO::ed25519_sign(t.sig.data(), (const uint8_t*)msg.data(), msg.size(), t.sk.data());
    return t;
  }
  bool verify() const {
    string msg = id + payload + to_string(expiry);
    return CRYPTO::ed25519_verify(sig.data(), (const uint8_t*)msg.data(), msg.size(), pk.data());
  }
};
struct Bundle { string bid; Token tok; uint64_t expiry; bool custody=true;
  static Bundle make(const Token& t){ return { util::h64("B"+t.id), t, t.expiry, true }; }
};
static vector<uint8_t> tok2bytes(const Token& t){
  vector<uint8_t> o; auto putS=[&](const string& s){ if(s.size()>UINT32_MAX) throw length_error("tok2bytes: string too large"); uint32_t L=static_cast<uint32_t>(s.size()); for(int i=0;i<4;++i) o.push_back((L>>(8*i))&0xFF); o.insert(o.end(), s.begin(), s.end()); };
  putS(t.id); putS(t.payload); for(int i=0;i<8;++i) o.push_back((t.created>>(8*i))&0xFF); for(int i=0;i<8;++i) o.push_back((t.expiry>>(8*i))&0xFF);
  o.insert(o.end(), t.pk.begin(), t.pk.end()); o.insert(o.end(), t.sig.begin(), t.sig.end()); return o;
}
static Token bytes2tok(const vector<uint8_t>& v){
  Token t;
  size_t off = 0;

  auto need = [&](size_t n) {
    if (off + n > v.size()) {
      throw std::runtime_error("bytes2tok: buffer too small");
    }
  };

  auto getS=[&](string& s){
    need(4);
    uint32_t L=0; for(int i=0;i<4;++i) L|=((uint32_t)v[off+i])<<(8*i); off+=4;
    need(L);
    s.assign((const char*)&v[off], (const char*)&v[off+L]); off+=L;
  };

  getS(t.id); getS(t.payload);

  need(8); t.created=0; for(int i=0;i<8;++i) t.created|=((uint64_t)v[off+i])<<(8*i); off+=8;
  need(8); t.expiry=0;  for(int i=0;i<8;++i) t.expiry |=((uint64_t)v[off+i])<<(8*i); off+=8;

  need(32 + 64);
  memcpy(t.pk.data(), &v[off], 32); off+=32; memcpy(t.sig.data(), &v[off], 64); off+=64;
  return t;
}

// ---------- Network state ----------
struct Node {
  enum class SendRefusal { NONE, EMPTY_BUFFER, NO_ELIGIBLE_PACKET, ENERGY, DUTY, HAL };
  struct SendResult {
    bool attempted = false;
    bool transmitted = false;
    bool delivered = false;
    fec::SegmentKind segment_kind = fec::SegmentKind::BULK;
    SendRefusal refusal = SendRefusal::NONE;
    aurora::safety::TransportAttemptTrace trace;
  };
  using HalTransmit = function<bool(phy::Mode, const fec::Pkt&)>;
  using PacketEligibility = function<bool(const fec::Pkt&)>;

  string id; geom::Vec2 pos;
  energy::Store bat{10.0, 6.0};
  vector<fec::Pkt> buf, inbox; unordered_set<string> seen;
  double harvest_W = 0.2;
  size_t tx_idx = 0; // NEW: rotating cursor to avoid resending same packet
  HAL::SimulationDutyLimiter simulated_rf_duty;

  static string packet_key(const fec::Pkt& packet) {
    return packet.generation_id + ":" + to_string(packet.segment_id) + ":" + to_string(packet.fp.seed);
  }
  void tick(double dt){ bat.harvest(harvest_W, dt); }
  void ingest(){ for(auto&p: inbox) { auto key=packet_key(p); if(!seen.count(key)){ buf.push_back(p); seen.insert(move(key));} } inbox.clear(); }
  void configure_simulation_duty(double duty_frac){ simulated_rf_duty.configure(duty_frac); }
  double duty_remaining_fraction(uint64_t now_ms){ return simulated_rf_duty.remaining_fraction(now_ms); }
  double duty_remaining_seconds(uint64_t now_ms){ return simulated_rf_duty.remaining_s(now_ms); }

  SendResult send_one(world::World& W, Node& rx, phy::Mode m,
                      uint64_t simulated_now_ms=0, bool critical_only=false,
                      const HalTransmit& injected_hal={},
                      const PacketEligibility& eligible={}){
    SendResult result;
    if(buf.empty()) { result.refusal=SendRefusal::EMPTY_BUFFER; return result; }
    if(tx_idx >= buf.size()) tx_idx = 0; // ring safety
    size_t selected = buf.size();
    for(size_t checked=0; checked<buf.size(); ++checked){
      const size_t candidate=(tx_idx+checked)%buf.size();
      if((!critical_only || buf[candidate].kind==fec::SegmentKind::CRITICAL) &&
         (!eligible || eligible(buf[candidate]))){ selected=candidate; break; }
    }
    if(selected==buf.size()){ result.refusal=SendRefusal::NO_ELIGIBLE_PACKET; return result; }
    auto pkt = buf[selected];
    tx_idx = (selected + 1) % buf.size();
    result.attempted = true;
    result.segment_kind = pkt.kind;
    auto& attempt = result.trace;
    attempt.simulated_now_ms = simulated_now_ms;
    attempt.packet_sequence = pkt.seq;
    attempt.symbol_seed = pkt.fp.seed;
    attempt.segment_id = pkt.segment_id;
    attempt.critical = pkt.kind == fec::SegmentKind::CRITICAL;
    attempt.attempted = true;

    size_t B = pkt.fp.data.size()+8;
    double J = phy::Jpkt(m,B);
    const double rf_airtime = HAL::LoraAirtimeSeconds(pkt.fp.data.size());
    attempt.energy_before_j = bat.E;
    attempt.energy_after_j = bat.E;
    attempt.energy_cost_j = J;
    attempt.duty_before_s = simulated_rf_duty.remaining_s(simulated_now_ms);
    attempt.duty_after_s = attempt.duty_before_s;
    attempt.rf_airtime_s = m==phy::Mode::RF ? rf_airtime : 0.0;
    auto refuse = [&](SendRefusal refusal,
                      aurora::safety::AttemptRefusal replay_refusal) {
      result.refusal = refusal;
      attempt.refusal = replay_refusal;
      attempt.energy_after_j = bat.E;
      attempt.duty_after_s = simulated_rf_duty.remaining_s(simulated_now_ms);
      return result;
    };
    if(!bat.can_spend(J)){
      return refuse(SendRefusal::ENERGY, aurora::safety::AttemptRefusal::ENERGY);
    }
    if(m==phy::Mode::RF &&
       !simulated_rf_duty.can_allow(simulated_now_ms, rf_airtime)){
      return refuse(SendRefusal::DUTY, aurora::safety::AttemptRefusal::DUTY);
    }

    const auto default_hal = [&](phy::Mode mode, const fec::Pkt& packet){
      attempt.hal_evaluated = true;
      if(mode==phy::Mode::RF){
        const auto hal = HAL::LORA_TX_SIMULATION_TRACE(
          packet.fp.data.data(), packet.fp.data.size());
        attempt.hal_replayable = hal.replayable;
        attempt.lbt_evaluated = true;
        attempt.lbt_threshold_dbm = hal.lbt.threshold_dbm;
        attempt.lbt_first_rssi_dbm = hal.lbt.first_rssi_dbm;
        attempt.lbt_second_valid = hal.lbt.second_valid;
        attempt.lbt_second_rssi_dbm = hal.lbt.second_rssi_dbm;
        return hal.accepted;
      }
#ifdef FIELD_BUILD
      attempt.hal_replayable = false;
#else
      attempt.hal_replayable = true;
#endif
      if(mode==phy::Mode::IR){
        return HAL::IR_TX(packet.fp.data.data(), packet.fp.data.size(), 3500);
      }
      vector<uint8_t> bits(packet.fp.data.size()*8, 1);
      return HAL::BS_MODULATE(bits.data(), bits.size(), 450);
    };
    bool hal_ok = false;
    if(injected_hal){
      attempt.hal_evaluated = true;
      attempt.hal_replayable = false;
      hal_ok = injected_hal(m, pkt);
    } else {
      hal_ok = default_hal(m, pkt);
    }
    attempt.hal_accepted = hal_ok;
    if(!hal_ok){
      return refuse(SendRefusal::HAL, aurora::safety::AttemptRefusal::HAL);
    }

    if(!bat.spend(J)) {
      return refuse(SendRefusal::ENERGY, aurora::safety::AttemptRefusal::ENERGY);
    }
    if(m==phy::Mode::RF &&
       !simulated_rf_duty.consume(simulated_now_ms, rf_airtime)){
      bat.E += J;
      return refuse(SendRefusal::DUTY, aurora::safety::AttemptRefusal::DUTY);
    }
    result.transmitted = true;
    attempt.transmitted = true;
    attempt.refusal = aurora::safety::AttemptRefusal::NONE;
    double g  = W.multibounce_best(pos, rx.pos, 2);
    double SNR= phy::snr_db(g,m,W.illum);
    double coding_gain = (m==phy::Mode::RF? 8.0 : m==phy::Mode::IR? 3.0 : 4.0);
    const auto channel_outcome = channel::pass_realistic_trace(
      SNR, m, coding_gain, /*rician*/ false);
    attempt.channel_evaluated = true;
    attempt.channel_snr_db = channel_outcome.snr_db;
    attempt.channel_coding_gain_db = channel_outcome.coding_gain_db;
    attempt.channel_fading_db = channel_outcome.fading_db;
    attempt.channel_threshold_db = channel_outcome.threshold_db;
    result.delivered = channel_outcome.delivered;
    attempt.delivered = result.delivered;
    attempt.energy_after_j = bat.E;
    attempt.duty_after_s = simulated_rf_duty.remaining_s(simulated_now_ms);
    if(result.delivered && !rx.seen.count(packet_key(pkt))) rx.inbox.push_back(pkt);
    return result;
  }
};

struct Net { world::World W; vector<unique_ptr<Node>> nodes;
  Node& add(const string& id, geom::Vec2 p){ nodes.emplace_back(make_unique<Node>()); nodes.back()->id=id; nodes.back()->pos=p; return *nodes.back(); }
  Node* get(const string& id){ for(auto& n:nodes) if(n->id==id) return n.get(); return nullptr; }
};

// ---------- Engine ----------
using FlowHealth = cl::FlowHealth;

struct Engine {
  Net net; Intention I, active_generation_contract;
  string token_id, bundle_id, generation_id; int K;
  size_t T=128;  // mantenuto 128 per FEC piu rapido
  size_t payload_size;
  aurora::transport::GenerationDescriptor generation_descriptor;
  aurora::transport::DecodeReport last_decode_report;
  TelemetrySink telemetry;
  
  // FASE 4: Organismo adattivo e health tracking
  unique_ptr<aurora::AuroraOrganism> organism;
  
  // FASE 4: FlowHealth per classe
  FlowHealth nerve_health_;
  FlowHealth gland_health_;
  FlowHealth muscle_health_;
  
  // FASE 4: SafetyMonitor
  aurora::safety::SafetyMonitor safety_monitor;
  aurora::safety::SafetyEnvelope safety_envelope;
  aurora::telemetry::DecisionReplayLog decision_trace_log;
  aurora::telemetry::SimulationEventLedger simulation_event_log;
  aurora::simulation::ContactSchedule contact_schedule =
    aurora::simulation::ContactSchedule::always_available();
  aurora::simulation::GenerationArrivalSchedule generation_arrival_schedule =
    aurora::simulation::GenerationArrivalSchedule::single_immediate();
  aurora::simulation::GenerationSchedulingPolicy generation_scheduling_policy;

  struct ScheduledGeneration {
    aurora::simulation::GenerationArrival arrival;
    Token token;
    Bundle bundle;
    vector<uint8_t> payload;
    aurora::transport::GenerationIdentity identity;
    aurora::transport::GenerationDescriptor descriptor;
    Intention contract;
    aurora::transport::DecodeReport report;
    vector<fec::Pkt> pending_packets;
    int required_rank = 0;
    aurora::transport::TransportImportance scheduling_importance =
      aurora::transport::TransportImportance::IMPORTANT;
    bool planned = false;
    bool arrived = false;
    bool terminal = false;
    bool delivered = false;
    optional<uint64_t> last_scheduled_at_ms;
    optional<uint64_t> last_effective_service_at_ms;
    uint64_t scheduled_turns = 0;
    uint64_t effective_service_attempts = 0;
  };
  vector<ScheduledGeneration> scheduled_generations;
  size_t active_generation_index = 0;
  
  // T1: Flag per stream interattivo e stato corrente
  bool interactive_stream_ = false;
  aurora::safety::SafetyState current_safety_state_ = aurora::safety::SafetyState::NO_EVIDENCE;
  cl::Mode current_mode_ = cl::Mode::NORMAL;
  
  Engine() : safety_monitor(aurora::safety::SafetyConfig::default_config()) {
    // FASE 4: Inizializza organismo
    organism = make_unique<aurora::AlienFountainOrganism>();
  }

  static uint64_t scheduled_expiry_ms(const ScheduledGeneration& generation) {
    const auto deadline_ms = generation.contract.deadline_ms();
    return deadline_ms > numeric_limits<uint64_t>::max() -
        generation.arrival.arrives_at_ms
      ? numeric_limits<uint64_t>::max()
      : generation.arrival.arrives_at_ms + deadline_ms;
  }

  static aurora::transport::TransportImportance descriptor_importance(
      const ScheduledGeneration& generation) {
    if (generation.arrival.service_class !=
        aurora::simulation::GenerationServiceClass::INHERIT) {
      return aurora::simulation::resolve_service_class(
        generation.arrival.service_class,
        aurora::transport::TransportImportance::IMPORTANT);
    }
    if (generation.descriptor.segments.empty()) {
      return generation.contract.importance;
    }
    return min_element(
      generation.descriptor.segments.begin(),
      generation.descriptor.segments.end(),
      [](const auto& left, const auto& right) {
        return static_cast<uint8_t>(left.importance) <
          static_cast<uint8_t>(right.importance);
      })->importance;
  }

  void init(const string& intention){
    I = Intention::parse(intention);
    util::rng.reseed(I.experiment_seed);
    net.W.obs.push_back({{0.52,0.52}, 0.22});
    for(int i=0;i<I.ris_tiles; ++i){ double u=(i+1.0)/(I.ris_tiles+1.0); net.W.ris.push_back({{0.12+0.78*u, 0.12+0.78*u}, (uint8_t)(i%4)}); }
    HAL::RADIO_INIT();
    HAL::LORA_CFG(phy::EU868_CH[0], 125, 12, 5, 12);
    Node& source = net.add("SRC",{0.06,0.08});
    source.configure_simulation_duty(I.duty_frac);
    net.add("DST",{0.94,0.92});

    const uint64_t deterministic_epoch_s = 1'700'000'000ULL + (I.experiment_seed % 1'000'000ULL);
    scheduled_generations.clear();
    scheduled_generations.reserve(generation_arrival_schedule.arrivals().size());
    for (const auto& arrival : generation_arrival_schedule.arrivals()) {
      Intention generation_contract = I;
      if (arrival.service_class !=
          aurora::simulation::GenerationServiceClass::INHERIT) {
        generation_contract.importance =
          aurora::simulation::resolve_service_class(
            arrival.service_class, generation_contract.importance);
      }
      if (arrival.deadline_ms != 0) {
        generation_contract.deadline_s =
          static_cast<double>(arrival.deadline_ms) / 1000.0;
        for (auto& segment : generation_contract.segments) {
          segment.deadline_ms = min(
            segment.deadline_ms, arrival.deadline_ms);
        }
      }
      generation_contract.validate();
      const string token_payload =
        "ACCESS:TEMP_KEY=abc123;ZONE=42;TTL=24h;CLASS=NORM;ARRIVAL=" +
        arrival.tag + ";";
      Token token = Token::make(
        token_payload, 24*3600,
        deterministic_epoch_s + arrival.arrives_at_ms / 1000ULL);
      Bundle bundle = Bundle::make(token);
      auto bytes = tok2bytes(token);
      ScheduledGeneration generation;
      generation.arrival = arrival;
      generation.token = std::move(token);
      generation.bundle = std::move(bundle);
      generation.payload = std::move(bytes);
      generation.contract = std::move(generation_contract);
      generation.identity = organism->reserve_identity(
        generation.contract, generation.token.id, generation.payload);
      if (arrival.service_class !=
          aurora::simulation::GenerationServiceClass::INHERIT) {
        generation.scheduling_importance =
          aurora::simulation::resolve_service_class(
            arrival.service_class,
            aurora::transport::TransportImportance::IMPORTANT);
      } else {
        generation.scheduling_importance = generation.contract.importance;
      }
      scheduled_generations.push_back(std::move(generation));
    }
    begin_simulation_event_session(source, *net.get("DST"));
    cout << "[GENERATION_SCHEDULE] arrivals=" << scheduled_generations.size()
         << " fingerprint=" << generation_arrival_schedule.fingerprint()
         << " discipline="
         << (generation_scheduling_policy.discipline ==
                aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR
              ? "fair" : "strict")
         << " quantum_ms="
         << generation_scheduling_policy.service_quantum_ms
         << " aging_ms=" << generation_scheduling_policy.aging_interval_ms
         << " starvation_ms="
         << generation_scheduling_policy.starvation_limit_ms
         << " maximum_scheduling_turn_gap_ms=";
    if (generation_scheduling_policy.discipline ==
        aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR) {
      cout << aurora::simulation::maximum_scheduling_turn_gap_ms(
        generation_scheduling_policy, scheduled_generations.size());
    } else {
      cout << "unbounded";
    }
    cout << endl;
  }

  void activate_generation(size_t index) {
    if (index >= scheduled_generations.size()) {
      throw logic_error("active generation index is out of range");
    }
    if (!scheduled_generations[index].planned ||
        !scheduled_generations[index].arrived) {
      throw logic_error("active generation has not been planned at arrival");
    }
    active_generation_index = index;
    const auto& generation = scheduled_generations[index];
    token_id = generation.token.id;
    bundle_id = generation.bundle.bid;
    generation_id = generation.descriptor.generation_id;
    generation_descriptor = generation.descriptor;
    active_generation_contract = generation.contract;
    last_decode_report = generation.report;
    K = generation.required_rank;
    payload_size = generation.payload.size();
  }

  pair<uint32_t, uint64_t> release_scheduled_arrival(
      uint64_t simulated_now_ms, Node& source) {
    for (size_t index = 0; index < scheduled_generations.size(); ++index) {
      auto& generation = scheduled_generations[index];
      if (!generation.arrived &&
          generation.arrival.arrives_at_ms == simulated_now_ms) {
        auto spawned = organism->spawn_reserved(
          generation.identity, generation.contract, generation.token.id,
          generation.payload, T, simulated_now_ms);
        generation.descriptor = std::move(spawned.descriptor);
        generation.pending_packets = std::move(spawned.packets);
        generation.required_rank = spawned.K;
        generation.scheduling_importance = descriptor_importance(generation);
        simulation_event_log.plan_generation(index, {
          generation.arrival.arrives_at_ms,
          generation.arrival.tag,
          generation.descriptor.generation_id,
          aurora::telemetry::simulation_descriptor_identity(
            generation.descriptor),
          static_cast<uint32_t>(generation.required_rank),
          generation.pending_packets.size(),
          generation.scheduling_importance,
          generation.descriptor.expires_at_ms});
        generation.planned = true;
        const auto count = generation.pending_packets.size();
        for (auto& packet : generation.pending_packets) {
          source.buf.push_back(std::move(packet));
        }
        generation.pending_packets.clear();
        generation.arrived = true;
        cout << "[GENERATION_ARRIVAL] index=" << index
             << " tag=" << generation.arrival.tag
             << " at_ms=" << simulated_now_ms
             << " id=" << generation.descriptor.generation_id
             << " packets=" << count << endl;
        return {static_cast<uint32_t>(index), count};
      }
    }
    return {aurora::telemetry::SimulationStepEvent::no_generation_arrival, 0};
  }

  void select_active_generation(uint64_t simulated_now_ms) {
    vector<aurora::simulation::GenerationSchedulingCandidate> candidates;
    candidates.reserve(scheduled_generations.size());
    for (size_t index = 0; index < scheduled_generations.size(); ++index) {
      const auto& generation = scheduled_generations[index];
      candidates.push_back({
        index,
        generation.arrival.arrives_at_ms,
        generation.planned
          ? generation.descriptor.expires_at_ms
          : scheduled_expiry_ms(generation),
        generation.scheduling_importance,
        generation.arrived,
        generation.terminal,
        generation.last_scheduled_at_ms});
    }
    if (const auto selected =
          aurora::simulation::select_scheduled_generation(
            candidates, simulated_now_ms, generation_scheduling_policy)) {
      activate_generation(*selected);
      auto& generation = scheduled_generations[*selected];
      generation.last_scheduled_at_ms = simulated_now_ms;
      ++generation.scheduled_turns;
      return;
    }
    for (size_t index = scheduled_generations.size(); index-- > 0;) {
      if (scheduled_generations[index].arrived &&
          scheduled_generations[index].arrival.arrives_at_ms <= simulated_now_ms) {
        activate_generation(index);
        return;
      }
    }
    throw logic_error("no scheduled generation is active");
  }

  bool all_generations_terminal() const {
    return all_of(scheduled_generations.begin(), scheduled_generations.end(),
      [](const auto& generation) {
        return generation.arrived && generation.terminal;
      });
  }

  void begin_simulation_event_session(const Node& source,
                                      const Node& destination) {
    aurora::telemetry::SimulationEventSession session;
    session.experiment_seed = I.experiment_seed;
    session.initial_random_state = util::rng.s;
    session.initial_source_buffer = source.buf.size();
    session.source_energy_capacity_j = source.bat.cap_J;
    session.source_initial_energy_j = source.bat.E;
    session.source_harvest_w = source.harvest_W;
    session.destination_energy_capacity_j = destination.bat.cap_J;
    session.destination_initial_energy_j = destination.bat.E;
    session.destination_harvest_w = destination.harvest_W;
    session.source_position = {source.pos.x, source.pos.y};
    session.destination_position = {destination.pos.x, destination.pos.y};
    session.ris_positions.reserve(net.W.ris.size());
    for (const auto& tile : net.W.ris) {
      session.ris_positions.push_back({tile.p.x, tile.p.y});
    }
    session.obstacles.reserve(net.W.obs.size());
    for (const auto& obstacle : net.W.obs) {
      session.obstacles.push_back({
        {obstacle.first.x, obstacle.first.y}, obstacle.second});
    }
    session.contact_schedule = contact_schedule;
    session.generation_arrival_schedule = generation_arrival_schedule;
    session.generation_scheduling_policy = generation_scheduling_policy;
    session.generations.reserve(scheduled_generations.size());
    for (const auto& generation : scheduled_generations) {
      session.generations.push_back({
        generation.arrival.arrives_at_ms,
        generation.arrival.tag,
        generation.identity.generation_id,
        0,
        0,
        0,
        generation.scheduling_importance,
        scheduled_expiry_ms(generation)});
    }
    simulation_event_log.begin(std::move(session));
  }

  aurora::telemetry::SimulationStepEvent begin_simulation_step(
      int step, Node& source, Node& destination,
      uint32_t arrived_generation_index,
      uint64_t arrived_source_packets) {
    aurora::telemetry::SimulationStepEvent event;
    event.step = static_cast<uint64_t>(step);
    event.simulated_now_ms = static_cast<uint64_t>(step) * 1000ULL;
    event.active_generation_index =
      static_cast<uint32_t>(active_generation_index);
    event.arrived_generation_index = arrived_generation_index;
    event.arrived_source_packets = arrived_source_packets;
    event.contact_available = contact_schedule.availability_at(
      event.simulated_now_ms);
    event.random_before = util::rng.s;
    event.source_energy_before_tick_j = source.bat.E;
    event.destination_energy_before_tick_j = destination.bat.E;
    event.source_buffer_before = source.buf.size();
    event.destination_buffer_before = destination.buf.size();
    event.destination_inbox_before = destination.inbox.size();
    event.decoder_rank_before = last_decode_report.decoder_rank;

    for (auto& node : net.nodes) {
      node->tick(1.0);
      node->ingest();
    }
    event.source_energy_after_tick_j = source.bat.E;
    event.destination_energy_after_tick_j = destination.bat.E;
    event.destination_buffer_after_ingest = destination.buf.size();
    event.destination_inbox_after_ingest = destination.inbox.size();

    event.entropy_residual = entropy_residual(
      static_cast<int>(event.decoder_rank_before), K);
    event.ris_phases.reserve(net.W.ris.size());
    for (auto& tile : net.W.ris) {
      const double angle =
        atan2(tile.p.y-source.pos.y, tile.p.x-source.pos.x) +
        atan2(destination.pos.y-tile.p.y, destination.pos.x-tile.p.x);
      const double jitter =
        event.entropy_residual * (util::rng.uni()-0.5) * 0.9;
      const double phase = fmod(angle + jitter, 2*M_PI);
      const int index = static_cast<int>(llround(phase/(M_PI/2.0))) & 3;
      tile.phase2b = static_cast<uint8_t>(index);
      event.ris_phases.push_back(tile.phase2b);
    }
    event.random_after_ris = util::rng.s;
    HAL::RIS_SET_PHASES(event.ris_phases);

    event.illumination = clamp(
      (event.entropy_residual - 0.10) / 0.25, 0.0, 1.0);
    net.W.illum = event.illumination;
#ifdef FIELD_BUILD
    if (net.W.illum > 0.0) {
      if (!HAL::CW_ON(0.05)) net.W.illum = 0.0;
    } else {
      HAL::CW_OFF();
    }
    event.illumination = net.W.illum;
#endif

    event.world_gain = net.W.multibounce_best(
      source.pos, destination.pos, 2);
    event.snr_rf_db = phy::snr_db(
      event.world_gain, phy::Mode::RF, event.illumination);
    event.snr_optical_db = phy::snr_db(
      event.world_gain, phy::Mode::IR, event.illumination);
    event.snr_backscatter_db = phy::snr_db(
      event.world_gain, phy::Mode::BACKSCATTER, event.illumination);
    return event;
  }

  void finish_simulation_step(
      aurora::telemetry::SimulationStepEvent event,
      const Node& source,
      const Node& destination,
      const aurora::transport::DecodeReport& report,
      uint32_t effective_service_attempts) {
    event.effective_transport_service_attempts = effective_service_attempts;
    event.random_after_action = util::rng.s;
    event.source_energy_after_action_j = source.bat.E;
    event.destination_energy_after_action_j = destination.bat.E;
    event.source_buffer_after_action = source.buf.size();
    event.destination_buffer_after_action = destination.buf.size();
    event.destination_inbox_after_action = destination.inbox.size();
    event.decoder_rank_after = report.decoder_rank;
    event.decode_status = static_cast<uint8_t>(report.status);
    simulation_event_log.record(event);
  }

  void record_effective_transport_service(
      uint64_t simulated_now_ms, uint32_t hal_accepted_attempts) {
    if (hal_accepted_attempts == 0) return;
    auto& generation = scheduled_generations[active_generation_index];
    generation.last_effective_service_at_ms = simulated_now_ms;
    generation.effective_service_attempts += hal_accepted_attempts;
  }

  static double entropy_residual(int have, int need){ double e=max(0, need-have)/(double)need; return min(1.0,max(0.0,e)); }

  bool has_critical_segments() const {
    return any_of(generation_descriptor.segments.begin(), generation_descriptor.segments.end(),
      [](const auto& segment){
        return segment.importance == aurora::transport::TransportImportance::CRITICAL;
      });
  }

  aurora::safety::TransportState transport_state(
      Node& source,
      uint64_t simulated_now_ms,
      const aurora::simulation::ContactAvailability& contact) const {
    aurora::safety::TransportState observed;
    observed.observed_at_ms = simulated_now_ms;
    observed.now_ms = simulated_now_ms;
    observed.source_energy_reserve = source.bat.soc();
    observed.rf_duty_remaining = source.duty_remaining_fraction(simulated_now_ms);
    observed.source_energy_capacity_j = source.bat.cap_J;
    const size_t packet_bytes = generation_descriptor.symbol_size + 8;
    observed.rf_energy_cost_per_attempt_j = phy::Jpkt(phy::Mode::RF, packet_bytes);
    observed.optical_energy_cost_per_attempt_j = phy::Jpkt(phy::Mode::IR, packet_bytes);
    observed.backscatter_energy_cost_per_attempt_j =
      phy::Jpkt(phy::Mode::BACKSCATTER, packet_bytes);
    observed.rf_duty_remaining_s = source.duty_remaining_seconds(simulated_now_ms);
    observed.rf_airtime_per_attempt_s =
      HAL::LoraAirtimeSeconds(generation_descriptor.symbol_size);
    observed.rf_contact_available = contact.rf;
    observed.optical_contact_available = contact.optical;
    observed.backscatter_contact_available = contact.backscatter;
    if (const auto runtime = organism->runtime_state(generation_id, simulated_now_ms)) {
      observed.emitted_symbols = runtime->emitted_symbols;
      observed.critical_emitted_symbols = runtime->critical_emitted_symbols;
      for (const auto& segment : runtime->segments) {
        observed.segments.push_back({
          segment.segment_id,
          segment.emitted_symbols,
          segment.decoder_rank,
          segment.complete,
          segment.expired});
      }
    }
    observed.decoder_rank = last_decode_report.decoder_rank;
    observed.required_rank = static_cast<uint32_t>(K);
    return observed;
  }

  struct ActionExecutionSummary {
    int attempts = 0;
    int hal_accepted = 0;
    int delivered = 0;
    string mode;
  };

  ActionExecutionSummary execute_decision(
      Node& source,
      Node& destination,
      telem::ChannelState& channel_state,
      aurora::safety::TransportDecisionTrace& trace,
      uint64_t simulated_now_ms,
      int min_spacing_ms,
      int jitter_ms,
      int step,
      bool debug_steps) {
    ActionExecutionSummary summary;
    const auto mode = phy_link(trace.decision.link);
    summary.mode = mode_to_string(mode);

    if (trace.decision.permitted && trace.decision.repair_symbols > 0) {
      auto repairs = organism->emit_repairs(
        generation_id,
        trace.decision.repair_symbols,
        trace.decision.critical_only,
        simulated_now_ms);
      if (repairs.emitted_symbols != trace.decision.repair_symbols) {
        throw logic_error("runtime repair emission disagrees with SafetyEnvelope decision");
      }
      for (auto& packet : repairs.packets) {
        source.buf.push_back(std::move(packet));
      }
    }

    auto do_sleep = [&](int base_ms, int random_ms){
      if(base_ms<=0 && random_ms<=0) return;
      int extra = random_ms>0 ? static_cast<int>(util::rng.uni()*random_ms) : 0;
      this_thread::sleep_for(std::chrono::milliseconds(base_ms + extra));
    };

    for(uint32_t i=0; i<trace.decision.transmission_attempts; ++i){
      const auto eligible = [&](const fec::Pkt& packet) {
        if(packet.generation_id != generation_id) return false;
        if(packet.segment_id >= generation_descriptor.segments.size()) return false;
        const auto& segment = generation_descriptor.segments[packet.segment_id];
        if(simulated_now_ms > segment.expires_at_ms) return false;
        if(packet.segment_id < trace.observed.segments.size()) {
          const auto& runtime = trace.observed.segments[packet.segment_id];
          if(runtime.complete || runtime.expired) return false;
        }
        return true;
      };
      const auto sent = source.send_one(
        net.W, destination, mode, simulated_now_ms,
        trace.decision.critical_only, {}, eligible);
      if(!sent.attempted) {
        throw logic_error("SafetyEnvelope admitted an action without an eligible packet");
      }
      trace.execution.attempts.push_back(sent.trace);
      if(sent.attempted) ++summary.attempts;
      if(sent.transmitted) ++summary.hal_accepted;
      if(sent.delivered) ++summary.delivered;
      if(trace.decision.critical_only && sent.attempted &&
         sent.segment_kind != fec::SegmentKind::CRITICAL) {
        throw logic_error("critical-only decision selected a non-critical packet");
      }
      channel_state.push_outcome(mode, sent.delivered);
      if(debug_steps && step < 3) {
        const double snr_world = phy::snr_db(
          net.W.multibounce_best(source.pos, destination.pos, 2), mode, net.W.illum);
        cout << "[STEP " << step << "] mode=" << summary.mode
             << " SNR(world)=" << fixed << setprecision(1) << snr_world
             << " outcome=" << (sent.delivered?"OK":"FAIL") << endl;
      }
      do_sleep(min_spacing_ms, jitter_ms);
    }

    trace.execution.recorded = true;
    trace.execution.link = trace.decision.link;
    trace.execution.transmission_attempts = static_cast<uint32_t>(summary.attempts);
    trace.execution.hal_accepted_attempts = static_cast<uint32_t>(summary.hal_accepted);
    trace.execution.delivered_attempts = static_cast<uint32_t>(summary.delivered);
    trace.execution.repair_symbols_emitted = trace.decision.permitted
      ? trace.decision.repair_symbols : 0;
    trace.execution.critical_only = trace.decision.critical_only;
    if (const auto error = trace.execution_error()) {
      throw logic_error("transport execution trace mismatch: " + *error);
    }
    return summary;
  }

  void update_controller_and_record(
      cl::Optimizer& optimizer,
      const aurora::safety::TransportDecisionTrace& decision_trace,
      const aurora::control::ProposalTransition& proposal_transition,
      const TelemetrySample& sample,
      uint64_t simulated_now_ms,
      const aurora::safety::SafetyMonitorSnapshot& before,
      cl::Mode mode_before) {
    aurora::safety::TelemetrySample safety_sample;
    safety_sample.observed_at_ms = simulated_now_ms;
    safety_sample.now_ms = simulated_now_ms;
    safety_sample.step = sample.step;
    safety_sample.have = sample.have;
    safety_sample.need = sample.need;
    safety_sample.mode = sample.mode;
    safety_sample.tries = sample.tries;
    safety_sample.successes = sample.successes;
    safety_sample.reward = sample.reward;
    safety_sample.snr_rf = sample.snr_rf;
    safety_sample.snr_ir = sample.snr_ir;
    safety_sample.snr_bs = sample.snr_bs;
    safety_sample.soc_src = sample.soc_src;
    safety_sample.duty_left = sample.duty_left;
    safety_sample.elapsed_s = sample.elapsed_s;
    safety_sample.nerve_fail_rate = sample.nerve_fail_rate;
    safety_sample.gland_fail_rate = sample.gland_fail_rate;
    safety_sample.muscle_fail_rate = sample.muscle_fail_rate;
    safety_sample.nerve_cov = sample.nerve_cov;
    safety_sample.gland_cov = sample.gland_cov;
    safety_sample.muscle_cov = sample.muscle_cov;
    safety_sample.nerve_bad_streak = sample.nerve_bad_streak;
    safety_sample.gland_bad_streak = sample.gland_bad_streak;
    safety_sample.muscle_bad_streak = sample.muscle_bad_streak;
    safety_sample.nerve_has_evidence = sample.nerve_has_evidence;
    safety_sample.gland_has_evidence = sample.gland_has_evidence;
    safety_sample.muscle_has_evidence = sample.muscle_has_evidence;

    const auto observation = aurora::safety::evidence_from(safety_sample);
    safety_monitor.observe(safety_sample);
    current_safety_state_ = safety_monitor.state();
    optimizer.update_mode(
      current_safety_state_, nerve_health_, gland_health_, muscle_health_);
    current_mode_ = optimizer.mode();

    aurora::control::ControllerTransition transition;
    transition.recorded = true;
    transition.now_ms = simulated_now_ms;
    transition.observation = observation;
    transition.before = before;
    transition.after = safety_monitor.snapshot();
    transition.mode_before = mode_before;
    transition.mode_after = optimizer.mode();
    decision_trace_log.record(
      active_generation_contract, generation_descriptor, decision_trace,
      proposal_transition, transition);
  }
  
  // T1: Emette evento JSON health su stdout
  void emit_health_event(int step, const FlowHealth& h, aurora::FlowClass cls) {
    if (!interactive_stream_) return;
    
    // Usa std::cerr per evitare problemi di buffering, oppure flush esplicito
    std::cout
      << "{"
      << "\"type\":\"health\","
      << "\"step\":" << step << ","
      << "\"class\":\"" << flowclass_to_string(cls) << "\","
      << "\"cov\":" << std::fixed << std::setprecision(4) << h.ewma_coverage << ","
      << "\"fail\":" << std::fixed << std::setprecision(4) << h.ewma_fail_rate << ","
      << "\"gs\":" << h.recent_good_streak << ","
      << "\"bs\":" << h.recent_bad_streak << ","
      << "\"safety\":\"" << safetystate_to_string(current_safety_state_) << "\","
      << "\"mode\":\"" << mode_to_string_cl(current_mode_) << "\""
      << "}"
      << std::endl;
    std::cout.flush();  // Force flush per assicurare output immediato
  }

  // T3: Ricarica config da file JSON (versione semplificata, non bloccante)
  void reload_interactive_config(const std::string& path) {
    // Per ora, salta il reload per evitare blocchi - userà sempre i default
    // TODO: implementare reload asincrono o più robusto
    (void)path; // Suppress unused warning
    
    // Versione semplificata: solo verifica che il file esista, ma non lo legge
    // La config viene scritta dalla dashboard Python, ma per ora usiamo i default
    return;
    
    // CODICE ORIGINALE COMMENTATO - CAUSA BLOCCAGGI
    /*
    try {
      using cl::AuroraInteractiveConfig;
      auto& cfg = cl::get_interactive_config();
      
      std::ifstream in(path);
      if (!in.is_open()) {
        return;
      }
      
      std::string content;
      std::string line;
      while (std::getline(in, line)) {
        content += line + "\n";
      }
      in.close();
    
    // Parser minimalissimo: cerca pattern "key":value
    // Per semplicità, usiamo un parser molto basico
    auto extract_double = [&](const std::string& key) -> bool {
      size_t pos = content.find("\"" + key + "\"");
      if (pos == std::string::npos) return false;
      pos = content.find(":", pos);
      if (pos == std::string::npos) return false;
      pos++;
      while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
      if (pos >= content.size()) return false;
      size_t end = pos;
      while (end < content.size() && content[end] != ',' && content[end] != '}' && content[end] != '\n') end++;
      std::string val_str = content.substr(pos, end - pos);
      try {
        double val = std::stod(val_str);
        if (key == "alpha_up") cfg.alpha_up = val;
        else if (key == "alpha_down") cfg.alpha_down = val;
        else if (key == "success_prob_nerve") cfg.success_prob_nerve = val;
        else if (key == "success_prob_gland") cfg.success_prob_gland = val;
        else if (key == "success_prob_muscle") cfg.success_prob_muscle = val;
        return true;
      } catch (...) {
        return false;
      }
    };
    
    auto extract_int = [&](const std::string& key) -> bool {
      size_t pos = content.find("\"" + key + "\"");
      if (pos == std::string::npos) return false;
      pos = content.find(":", pos);
      if (pos == std::string::npos) return false;
      pos++;
      while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;
      if (pos >= content.size()) return false;
      size_t end = pos;
      while (end < content.size() && content[end] != ',' && content[end] != '}' && content[end] != '\n') end++;
      std::string val_str = content.substr(pos, end - pos);
      try {
        int val = std::stoi(val_str);
        if (key == "panic_boost_steps") cfg.panic_boost_steps = val;
        return true;
      } catch (...) {
        return false;
      }
    };
    
      extract_double("alpha_up");
      extract_double("alpha_down");
      extract_int("panic_boost_steps");
      extract_double("success_prob_nerve");
      extract_double("success_prob_gland");
      extract_double("success_prob_muscle");
    } catch (const std::exception& e) {
      // Ignora
    } catch (...) {
      // Ignora
    }
    */
  }

  // DecodeReport is the sole input to transport health.
  void apply_flow_feedback(const aurora::transport::DecodeReport& report,
                           aurora::FlowClass flow_class) {
    FlowHealth* fh = nullptr;
    switch (flow_class) {
      case aurora::FlowClass::NERVE:  fh = &nerve_health_; break;
      case aurora::FlowClass::GLAND:  fh = &gland_health_; break;
      case aurora::FlowClass::MUSCLE: fh = &muscle_health_; break;
    }
    if (!fh) return;
    fh->observe(report);
    
    // Log sintetico
    string cls_name;
    switch (flow_class) {
      case aurora::FlowClass::NERVE:  cls_name = "NERVE"; break;
      case aurora::FlowClass::GLAND:  cls_name = "GLAND"; break;
      case aurora::FlowClass::MUSCLE: cls_name = "MUSCLE"; break;
    }
    cout << "[FLOW][HEALTH] class=" << cls_name
         << " delivered=" << (report.delivered() ? "true" : "false")
         << " cov=" << fixed << setprecision(2) << report.coverage
         << " ewma_cov=" << fh->ewma_coverage
         << " ewma_fail=" << fh->ewma_fail_rate
         << " gs=" << fh->recent_good_streak
         << " bs=" << fh->recent_bad_streak
         << endl;
  }

  bool run(){
    using HAL::FHSS_next;
    Node& S=*net.get("SRC"); Node& D=*net.get("DST");
    vector<vector<uint8_t>> used;
    bool delivered=false; vector<uint8_t> out;

    cl::Optimizer opt;
    opt.reseed_proposal(I.experiment_seed);
    telem::ChannelState chan;
    double epoch=1.0;

    // T3: Ricarica config all'inizio se in modalità interattiva
    if (interactive_stream_) {
      reload_interactive_config("aurora_interactive_config.json");
    }

    // Iterazioni massime (sufficienti anche per deadline lunghe)
    for(int step=0; step<500 && !all_generations_terminal(); ++step){
      // T3: Ricarica config periodicamente (ogni 20 step)
      if (interactive_stream_ && (step % 20 == 0)) {
        reload_interactive_config("aurora_interactive_config.json");
      }
      const uint64_t step_now_ms = static_cast<uint64_t>(step) * 1000ULL;
      const auto arrival = release_scheduled_arrival(step_now_ms, S);
      select_active_generation(step_now_ms);
      auto simulation_event = begin_simulation_step(
        step, S, D, arrival.first, arrival.second);
      int have=static_cast<int>(last_decode_report.decoder_rank);

      // progress
      if(step % 20 == 0 || have >= K) {
        cout << "[PROGRESS] step=" << step << " have=" << have << "/" << K 
             << " (" << fixed << setprecision(1) << (100.0*have/K) << "%)"
             << " SRC_SoC=" << setprecision(0) << (S.bat.soc()*100) << "%" << endl;
      }

      const uint64_t simulated_now_ms = simulation_event.simulated_now_ms;
      const double elapsed = static_cast<double>(simulated_now_ms) / 1000.0;

      // Stato per optimizer (channel-aware + priority)
      cl::NetworkState Sx{};
      Sx.soc_src=S.bat.soc(); Sx.symbols_have=have; Sx.symbols_need=K;
      Sx.deadline_left_s = generation_descriptor.expires_at_ms > simulated_now_ms
        ? static_cast<double>(generation_descriptor.expires_at_ms -
            simulated_now_ms) / 1000.0
        : 0.0;
      Sx.duty_left_rf=S.duty_remaining_fraction(simulated_now_ms);

      Sx.chan = chan;
      Sx.chan.push_snr(phy::Mode::RF, simulation_event.snr_rf_db);
      Sx.chan.push_snr(
        phy::Mode::BACKSCATTER, simulation_event.snr_backscatter_db);
      Sx.chan.push_snr(phy::Mode::IR, simulation_event.snr_optical_db);
      Sx.decode_rate_symps = (have>0? have / max(1.0, elapsed) : 0.0);

      // priority
      Sx.prio = (Sx.deadline_left_s < active_generation_contract.deadline_s*0.15 ? cl::Priority::CRITICAL
                : Sx.deadline_left_s < active_generation_contract.deadline_s*0.4 ? cl::Priority::NORMAL
                : cl::Priority::BULK);
      Sx.emergency_mode = (Sx.deadline_left_s < active_generation_contract.deadline_s*0.08 && (K-have) > (K*0.25));
      Sx.covert_seq = static_cast<uint16_t>((I.experiment_seed + static_cast<uint64_t>(step)) & 0xFF);

      const auto controller_before = safety_monitor.snapshot();
      const auto controller_mode_before = opt.mode();
      const auto proposal_input = opt.proposal_input(
        active_generation_contract, Sx, epoch, has_critical_segments());
      const auto proposal_before = opt.proposal_state();
      const auto proposal_decision = opt.propose(proposal_input);
      const auto proposal_after_derivation = opt.proposal_state();
      const auto proposed = proposal_decision.transport;
      const auto observed = transport_state(
        S, simulated_now_ms, simulation_event.contact_available);
      auto decision_trace = safety_envelope.constrain(
        active_generation_contract, generation_descriptor, observed, proposed);
      double hop = HAL::FHSS_next(proposal_decision.covert_sequence);
      HAL::LORA_CFG(
        hop, proposal_decision.rf_bandwidth_khz, 12, 5,
        proposal_decision.preamble_symbols);

      const uint8_t covert_seq = proposal_decision.covert_sequence;
      const bool emergency = proposal_decision.emergency;
      const auto executed = execute_decision(
        S, D, chan, decision_trace, simulated_now_ms,
        proposal_decision.minimum_spacing_ms,
        proposal_decision.jitter_ms, step, true);
      const int tries_real = executed.attempts;
      const int ok_cnt = executed.delivered;
      const string mode_chosen = executed.mode;
      record_effective_transport_service(
        simulated_now_ms, static_cast<uint32_t>(executed.hal_accepted));
      if(step < 3) std::cout << "[ADAPTIVE] step=" << step << " mode_chosen=" << mode_chosen << std::endl;

      double reward = clamp( (double)ok_cnt / max(1, tries_real), 0.0, 1.0 );
      aurora::control::ProposalFeedback proposal_feedback;
      proposal_feedback.applied = tries_real > 0;
      proposal_feedback.executed_link = decision_trace.decision.link;
      proposal_feedback.reward = proposal_feedback.applied ? reward : 0.0;
      opt.apply_feedback(proposal_feedback);
      aurora::control::ProposalTransition proposal_transition;
      proposal_transition.recorded = true;
      proposal_transition.before = proposal_before;
      proposal_transition.input = proposal_input;
      proposal_transition.decision = proposal_decision;
      proposal_transition.after_proposal = proposal_after_derivation;
      proposal_transition.feedback = proposal_feedback;
      proposal_transition.after = opt.proposal_state();
      if(emergency){ cout<<"[COVERT] EMERGENCY flag; seq="<<(int)covert_seq<<"\n"; }

      int have_after = 0; for (auto& p : D.buf) if (p.generation_id == generation_id) have_after++;
      
      // FASE 4: Integra organismo per ottenere risultati reali
      aurora::FlowProfile flow_profile = organism->build_profile(
        active_generation_contract);
      
      // Raccogli pacchetti ricevuti per questo token_id
      vector<fec::Pkt> received_packets;
      for (auto& p : D.buf) {
        if (p.generation_id == generation_id) {
          received_packets.push_back(p);
        }
      }
      
      auto& active_generation = scheduled_generations[active_generation_index];
      auto res = active_generation.terminal
        ? active_generation.report
        : organism->integrate(
            generation_id, received_packets, simulated_now_ms);
      last_decode_report = res;
      active_generation.report = res;
      if (!active_generation.terminal) {
        apply_flow_feedback(res, flow_profile.flow_class);
      }
      if (!active_generation.terminal && res.delivered()) {
        active_generation.terminal = true;
        active_generation.delivered = true;
        out = res.payload;
        used.clear();
        for (const auto& packet : received_packets) used.push_back(packet.fp.data);
        cout << "[SUCCESS] authoritative generation decode rank="
             << res.decoder_rank << "/" << res.required_rank << endl;
      }
      const bool became_expired = !active_generation.terminal &&
        res.status == aurora::transport::DecodeStatus::EXPIRED;
      if (became_expired) {
        active_generation.terminal = true;
      }
      
      TelemetrySample sample;
      sample.step = step;
      sample.have = have_after;
      sample.need = K;
      sample.mode = mode_chosen;
      sample.tries = tries_real;
      sample.successes = ok_cnt;
      sample.reward = reward;
      sample.snr_rf = Sx.chan.snr_est(phy::Mode::RF);
      sample.snr_ir = Sx.chan.snr_est(phy::Mode::IR);
      sample.snr_bs = Sx.chan.snr_est(phy::Mode::BACKSCATTER);
      sample.soc_src = S.bat.soc();
      sample.duty_left = Sx.duty_left_rf;
      sample.elapsed_s = elapsed;
      
      // FASE 4: Estendi TelemetrySample con FlowHealth metrics
      sample.nerve_fail_rate = nerve_health_.ewma_fail_rate;
      sample.gland_fail_rate = gland_health_.ewma_fail_rate;
      sample.muscle_fail_rate = muscle_health_.ewma_fail_rate;
      sample.nerve_cov = nerve_health_.ewma_coverage;
      sample.gland_cov = gland_health_.ewma_coverage;
      sample.muscle_cov = muscle_health_.ewma_coverage;
      sample.nerve_bad_streak = nerve_health_.recent_bad_streak;
      sample.gland_bad_streak = gland_health_.recent_bad_streak;
      sample.muscle_bad_streak = muscle_health_.recent_bad_streak;
      sample.nerve_has_evidence =
        nerve_health_.success_count + nerve_health_.fail_count > 0;
      sample.gland_has_evidence =
        gland_health_.success_count + gland_health_.fail_count > 0;
      sample.muscle_has_evidence =
        muscle_health_.success_count + muscle_health_.fail_count > 0;
      
      update_controller_and_record(
        opt, decision_trace, proposal_transition, sample, simulated_now_ms,
        controller_before, controller_mode_before);
      finish_simulation_step(
        simulation_event, S, D, res,
        static_cast<uint32_t>(executed.hal_accepted));

      emit_health_event(step, nerve_health_, aurora::FlowClass::NERVE);
      emit_health_event(step, gland_health_, aurora::FlowClass::GLAND);
      emit_health_event(step, muscle_health_, aurora::FlowClass::MUSCLE);
      
      telemetry.record(sample);

      if(became_expired){
          cout << "[TIMEOUT] Deadline exceeded at step " << step 
               << " (elapsed=" << fixed << setprecision(2) << elapsed 
               << "s, deadline=" << active_generation_contract.deadline_s
               << "s)" << endl;
      }

      epoch += 1.0;

      // **PATCH**: sleep adattivo in base al tempo rimanente
      {
        double dl_rem = Sx.deadline_left_s;
        int ms; if      (dl_rem < 2.0) ms = 2; else if (dl_rem < 5.0) ms = 6; else ms = 12;
        this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
    }

    delivered = all_generations_terminal() &&
      all_of(scheduled_generations.begin(), scheduled_generations.end(),
        [](const auto& generation) { return generation.delivered; });
    if(delivered){
      try {
        Token rx = bytes2tok(out);
        cout<<"DELIVERED \\xE2\\x9C\\x93 sig="<<(rx.verify()?"OK":"BAD")<<" payload="<<rx.payload<<"\n";
        vector<string> leaves; leaves.reserve(used.size()); for(auto& s:used) leaves.push_back(podm::leaf(s));
        string root = podm::root(leaves);
        array<uint8_t,32> pk; array<uint8_t,64> sk; CRYPTO::ed25519_keypair(pk.data(), sk.data());
        string msg = bundle_id + token_id + root + to_string(generation_descriptor.expires_at_ms);
        array<uint8_t,64> sig; CRYPTO::ed25519_sign(sig.data(), (const uint8_t*)msg.data(), msg.size(), sk.data());
        cout<<"PoD-M root="<<root.substr(0,16)<<"... sig="<<CRYPTO::b64(sig.data(),sig.size()).substr(0,16)<<"...\n";
      } catch (const std::exception& e) {
        std::cerr << "[ERROR] decode payload parse failed: " << e.what()
                  << " size=" << out.size() << "\n";
        delivered = false;
      }
    }
    if(!delivered){
      cout<<"FAILED - aumenta RIS/epsilon o budget duty.\n";
    }

    auto show=[&](const string& id){ Node& n=*net.get(id); size_t have=0; for(auto& p:n.buf) if(p.token_id==token_id) have++; cout<<" - "<<id<<" SoC="<<fixed<<setprecision(0)<<n.bat.soc()*100<<"% buf="<<have<<"\n"; };
    show("SRC"); show("DST");
    cout<<"RIS="<<net.W.ris.size()<<" illum="<<net.W.illum<<"\n";
    telemetry.flush();
    return delivered;
  }
};

bool aurora_run(const std::string& intention, Engine* outE = nullptr) {
  ios::sync_with_stdio(false);
  cout<<"=== AURORA-X - Extreme Field Orchestrator (UPDATED) ===\n";
  std::unique_ptr<Engine> owned_engine;
  Engine* E = outE;
  if (!E) {
    owned_engine = std::make_unique<Engine>();
    E = owned_engine.get();
  }
  E->init(intention);
  bool ok = E->run();
  cout<<(ok? ">>> SUCCESS\n" : ">>> INCOMPLETE - ritenta con piu RIS/epsilon\n");
  return ok;
}

// T2: Funzione per eseguire il lab interattivo
bool aurora_run_interactive_lab(Engine& engine, int max_steps = 5000) {
  // NON disabilitare sync per permettere flush immediato
  // ios::sync_with_stdio(false);  // COMMENTATO: causa problemi di buffering
  // Messaggio iniziale su stderr per non interferire con JSON su stdout
  std::cerr << "=== AURORA-X - Interactive Lab Mode ===" << std::endl;
  std::cerr << "Running for up to " << max_steps << " steps. Press Ctrl+C to stop." << std::endl;
  std::cerr << "Emitting JSON health events to stdout..." << std::endl;
  
  std::string intention = "deadline:600; reliability:0.99; duty:0.01; optical:on; backscatter:on; ris:16; selector:argmax";
  engine.init(intention);
  engine.interactive_stream_ = true;
  
  // Loop continuo per il lab
  using HAL::FHSS_next;
  Node& S=*engine.net.get("SRC"); Node& D=*engine.net.get("DST");
  
  vector<vector<uint8_t>> used;
  bool delivered=false; vector<uint8_t> out;

  cl::Optimizer opt;
  opt.reseed_proposal(engine.I.experiment_seed);
  telem::ChannelState chan;
  double epoch=1.0;

  // T3: Ricarica config all'inizio
  engine.reload_interactive_config("aurora_interactive_config.json");

  for(int step=0; step<max_steps && !delivered; ++step){
    
    // T3: Ricarica config periodicamente (ogni 20 step)
    if (step % 20 == 0) {
      engine.reload_interactive_config("aurora_interactive_config.json");
    }
    
    const uint64_t step_now_ms = static_cast<uint64_t>(step) * 1000ULL;
    const auto arrival = engine.release_scheduled_arrival(step_now_ms, S);
    engine.select_active_generation(step_now_ms);
    auto simulation_event = engine.begin_simulation_step(
      step, S, D, arrival.first, arrival.second);
    int have=static_cast<int>(engine.last_decode_report.decoder_rank);

    // progress (meno verboso in lab mode)
    if(step % 100 == 0 || have >= engine.K) {
      cout << "[PROGRESS] step=" << step << " have=" << have << "/" << engine.K 
           << " (" << fixed << setprecision(1) << (100.0*have/engine.K) << "%)"
           << " SRC_SoC=" << setprecision(0) << (S.bat.soc()*100) << "%" << endl;
    }

    const uint64_t simulated_now_ms = simulation_event.simulated_now_ms;
    const double elapsed = static_cast<double>(simulated_now_ms) / 1000.0;

    // Stato per optimizer
    cl::NetworkState Sx{};
    Sx.soc_src=S.bat.soc(); Sx.symbols_have=have; Sx.symbols_need=engine.K;
    Sx.deadline_left_s = engine.generation_descriptor.expires_at_ms >
        simulated_now_ms
      ? static_cast<double>(engine.generation_descriptor.expires_at_ms -
          simulated_now_ms) / 1000.0
      : 0.0;
    Sx.duty_left_rf=S.duty_remaining_fraction(simulated_now_ms);

    Sx.chan = chan;
    Sx.chan.push_snr(phy::Mode::RF, simulation_event.snr_rf_db);
    Sx.chan.push_snr(
      phy::Mode::BACKSCATTER, simulation_event.snr_backscatter_db);
    Sx.chan.push_snr(phy::Mode::IR, simulation_event.snr_optical_db);
    Sx.decode_rate_symps = (have>0? have / max(1.0, elapsed) : 0.0);

    Sx.prio = (Sx.deadline_left_s < engine.active_generation_contract.deadline_s*0.15 ? cl::Priority::CRITICAL
              : Sx.deadline_left_s < engine.active_generation_contract.deadline_s*0.4 ? cl::Priority::NORMAL
              : cl::Priority::BULK);
    Sx.emergency_mode = (Sx.deadline_left_s < engine.active_generation_contract.deadline_s*0.08 && (engine.K-have) > (engine.K*0.25));
    Sx.covert_seq = static_cast<uint16_t>((engine.I.experiment_seed + static_cast<uint64_t>(step)) & 0xFF);

    const auto controller_before = engine.safety_monitor.snapshot();
    const auto controller_mode_before = opt.mode();
    const auto proposal_input = opt.proposal_input(
      engine.active_generation_contract, Sx, epoch,
      engine.has_critical_segments());
    const auto proposal_before = opt.proposal_state();
    const auto proposal_decision = opt.propose(proposal_input);
    const auto proposal_after_derivation = opt.proposal_state();
    const auto proposed = proposal_decision.transport;
    const auto observed = engine.transport_state(
      S, simulated_now_ms, simulation_event.contact_available);
    auto decision_trace = engine.safety_envelope.constrain(
      engine.active_generation_contract, engine.generation_descriptor,
      observed, proposed);
    double hop = HAL::FHSS_next(proposal_decision.covert_sequence);
    HAL::LORA_CFG(
      hop, proposal_decision.rf_bandwidth_khz, 12, 5,
      proposal_decision.preamble_symbols);

    const uint8_t covert_seq = proposal_decision.covert_sequence;
    const bool emergency = proposal_decision.emergency;
    const auto executed = engine.execute_decision(
      S, D, chan, decision_trace, simulated_now_ms,
      proposal_decision.minimum_spacing_ms,
      proposal_decision.jitter_ms, step, false);
    const int tries_real = executed.attempts;
    const int ok_cnt = executed.delivered;
    const string mode_chosen = executed.mode;
    engine.record_effective_transport_service(
      simulated_now_ms, static_cast<uint32_t>(executed.hal_accepted));

    double reward = clamp( (double)ok_cnt / max(1, tries_real), 0.0, 1.0 );
    aurora::control::ProposalFeedback proposal_feedback;
    proposal_feedback.applied = tries_real > 0;
    proposal_feedback.executed_link = decision_trace.decision.link;
    proposal_feedback.reward = proposal_feedback.applied ? reward : 0.0;
    opt.apply_feedback(proposal_feedback);
    aurora::control::ProposalTransition proposal_transition;
    proposal_transition.recorded = true;
    proposal_transition.before = proposal_before;
    proposal_transition.input = proposal_input;
    proposal_transition.decision = proposal_decision;
    proposal_transition.after_proposal = proposal_after_derivation;
    proposal_transition.feedback = proposal_feedback;
    proposal_transition.after = opt.proposal_state();
    if(emergency){ cout<<"[COVERT] EMERGENCY flag; seq="<<(int)covert_seq<<"\n"; }

    int have_after = 0; for (auto& p : D.buf) if (p.generation_id == engine.generation_id) have_after++;
    
    // Integra organismo
    aurora::FlowProfile flow_profile = engine.organism->build_profile(
      engine.active_generation_contract);
    
    vector<fec::Pkt> received_packets;
    for (auto& p : D.buf) {
      if (p.generation_id == engine.generation_id) {
        received_packets.push_back(p);
      }
    }

    auto res = engine.organism->integrate(
      engine.generation_id, received_packets, simulated_now_ms);
    engine.last_decode_report = res;
    engine.apply_flow_feedback(res, flow_profile.flow_class);
    if (res.delivered()) {
      delivered = true;
      out = res.payload;
      used.clear();
      for (const auto& packet : received_packets) used.push_back(packet.fp.data);
    }
    
    TelemetrySample sample;
    sample.step = step;
    sample.have = have_after;
    sample.need = engine.K;
    sample.mode = mode_chosen;
    sample.tries = tries_real;
    sample.successes = ok_cnt;
    sample.reward = reward;
    sample.snr_rf = Sx.chan.snr_est(phy::Mode::RF);
    sample.snr_ir = Sx.chan.snr_est(phy::Mode::IR);
    sample.snr_bs = Sx.chan.snr_est(phy::Mode::BACKSCATTER);
    sample.soc_src = S.bat.soc();
    sample.duty_left = Sx.duty_left_rf;
    sample.elapsed_s = elapsed;
    
    sample.nerve_fail_rate = engine.nerve_health_.ewma_fail_rate;
    sample.gland_fail_rate = engine.gland_health_.ewma_fail_rate;
    sample.muscle_fail_rate = engine.muscle_health_.ewma_fail_rate;
    sample.nerve_cov = engine.nerve_health_.ewma_coverage;
    sample.gland_cov = engine.gland_health_.ewma_coverage;
    sample.muscle_cov = engine.muscle_health_.ewma_coverage;
    sample.nerve_bad_streak = engine.nerve_health_.recent_bad_streak;
    sample.gland_bad_streak = engine.gland_health_.recent_bad_streak;
    sample.muscle_bad_streak = engine.muscle_health_.recent_bad_streak;
    sample.nerve_has_evidence =
      engine.nerve_health_.success_count + engine.nerve_health_.fail_count > 0;
    sample.gland_has_evidence =
      engine.gland_health_.success_count + engine.gland_health_.fail_count > 0;
    sample.muscle_has_evidence =
      engine.muscle_health_.success_count + engine.muscle_health_.fail_count > 0;
    
    engine.update_controller_and_record(
      opt, decision_trace, proposal_transition, sample, simulated_now_ms,
      controller_before, controller_mode_before);
    engine.finish_simulation_step(
      simulation_event, S, D, res,
      static_cast<uint32_t>(executed.hal_accepted));

    engine.emit_health_event(step, engine.nerve_health_, aurora::FlowClass::NERVE);
    engine.emit_health_event(step, engine.gland_health_, aurora::FlowClass::GLAND);
    engine.emit_health_event(step, engine.muscle_health_, aurora::FlowClass::MUSCLE);
    
    engine.telemetry.record(sample);

    if (!delivered && res.status == aurora::transport::DecodeStatus::EXPIRED) {
      break;
    }

    epoch += 1.0;

    // Sleep adattivo
    {
      double dl_rem = Sx.deadline_left_s;
      int ms; if (dl_rem < 2.0) ms = 2; else if (dl_rem < 5.0) ms = 6; else ms = 12;
      this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
  }

  engine.telemetry.flush();
  return delivered;
}

#ifndef AURORA_NO_MAIN
int main(int argc, char* argv[]){
  // T2: Parsing argomenti per modalità interattiva
  bool interactive_lab = false;
  std::string decision_trace_path;
  std::string event_ledger_path;
  std::string contact_schedule_path;
  std::string contact_schedule_out_path;
  std::string generation_arrival_schedule_path;
  std::string generation_arrival_schedule_out_path;
  aurora::simulation::GenerationSchedulingPolicy generation_scheduling_policy;
  [[maybe_unused]] bool generation_scheduling_option_seen = false;
  const auto positive_option = [&](int& index, const char* option) {
    if (index + 1 >= argc) {
      throw invalid_argument(string(option) + " requires a positive integer");
    }
    const string text = argv[++index];
    if (text.empty() || text.front() == '-') {
      throw invalid_argument(string(option) + " requires a positive integer");
    }
    size_t consumed = 0;
    uint64_t value = 0;
    try {
      value = stoull(text, &consumed, 10);
    } catch (...) {
      throw invalid_argument(string(option) + " requires a positive integer");
    }
    if (consumed != text.size() || value == 0) {
      throw invalid_argument(string(option) + " requires a positive integer");
    }
    return value;
  };
  try {
    for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--interactive-lab") {
      interactive_lab = true;
    } else if (std::string(argv[i]) == "--decision-trace") {
      if (i + 1 >= argc) {
        std::cerr << "--decision-trace requires a file path\n";
        return 2;
      }
      decision_trace_path = argv[++i];
    } else if (std::string(argv[i]) == "--event-ledger") {
      if (i + 1 >= argc) {
        std::cerr << "--event-ledger requires a file path\n";
        return 2;
      }
      event_ledger_path = argv[++i];
    } else if (std::string(argv[i]) == "--contact-schedule") {
      if (i + 1 >= argc) {
        std::cerr << "--contact-schedule requires a file path\n";
        return 2;
      }
      contact_schedule_path = argv[++i];
    } else if (std::string(argv[i]) == "--contact-schedule-out") {
      if (i + 1 >= argc) {
        std::cerr << "--contact-schedule-out requires a file path\n";
        return 2;
      }
      contact_schedule_out_path = argv[++i];
    } else if (std::string(argv[i]) == "--generation-arrivals") {
      if (i + 1 >= argc) {
        std::cerr << "--generation-arrivals requires a file path\n";
        return 2;
      }
      generation_arrival_schedule_path = argv[++i];
    } else if (std::string(argv[i]) == "--generation-arrivals-out") {
      if (i + 1 >= argc) {
        std::cerr << "--generation-arrivals-out requires a file path\n";
        return 2;
      }
      generation_arrival_schedule_out_path = argv[++i];
    } else if (std::string(argv[i]) == "--generation-scheduler") {
      if (i + 1 >= argc) {
        throw invalid_argument(
          "--generation-scheduler requires strict or fair");
      }
      const string discipline = argv[++i];
      if (discipline == "strict") {
        generation_scheduling_policy.discipline =
          aurora::simulation::GenerationSchedulingDiscipline::STRICT_PRIORITY_EDF;
      } else if (discipline == "fair") {
        generation_scheduling_policy.discipline =
          aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR;
      } else {
        throw invalid_argument(
          "--generation-scheduler requires strict or fair");
      }
      generation_scheduling_option_seen = true;
    } else if (std::string(argv[i]) == "--generation-aging-ms") {
      generation_scheduling_policy.aging_interval_ms = positive_option(
        i, "--generation-aging-ms");
      generation_scheduling_option_seen = true;
    } else if (std::string(argv[i]) == "--generation-starvation-ms") {
      generation_scheduling_policy.starvation_limit_ms = positive_option(
        i, "--generation-starvation-ms");
      generation_scheduling_option_seen = true;
    }
    }
    generation_scheduling_policy.validate();
  } catch (const exception& error) {
    cerr << error.what() << '\n';
    return 2;
  }

  auto engine = std::make_unique<Engine>();
  engine->generation_scheduling_policy = generation_scheduling_policy;
#ifdef FIELD_BUILD
  if (!event_ledger_path.empty() || !contact_schedule_path.empty() ||
      !contact_schedule_out_path.empty() ||
      !generation_arrival_schedule_path.empty() ||
      !generation_arrival_schedule_out_path.empty() ||
      generation_scheduling_option_seen) {
    std::cerr << "event/contact/arrival/scheduler options are simulation-only and unavailable in FIELD_BUILD\n";
    return 2;
  }
#endif
  if (!contact_schedule_path.empty()) {
    try {
      engine->contact_schedule =
        aurora::simulation::ContactSchedule::load(contact_schedule_path);
    } catch (const std::exception& error) {
      std::cerr << "[CONTACT] failed to load schedule: "
                << error.what() << '\n';
      return 2;
    }
  }
  if (!generation_arrival_schedule_path.empty()) {
    try {
      engine->generation_arrival_schedule =
        aurora::simulation::GenerationArrivalSchedule::load(
          generation_arrival_schedule_path);
    } catch (const std::exception& error) {
      std::cerr << "[ARRIVAL] failed to load schedule: "
                << error.what() << '\n';
      return 2;
    }
  }
  if (engine->generation_scheduling_policy.discipline ==
      aurora::simulation::GenerationSchedulingDiscipline::AGING_FAIR) {
    try {
      (void)aurora::simulation::maximum_scheduling_turn_gap_ms(
        engine->generation_scheduling_policy,
        engine->generation_arrival_schedule.arrivals().size());
    } catch (const exception& error) {
      cerr << "[SCHEDULER] invalid policy: " << error.what() << '\n';
      return 2;
    }
  }
  if (interactive_lab && !generation_arrival_schedule_path.empty()) {
    std::cerr << "generation arrival replay is unavailable in interactive-lab mode\n";
    return 2;
  }
  bool ok = false;
  if (interactive_lab) {
    ok = aurora_run_interactive_lab(*engine, 5000);
  } else {
    std::string intention = "deadline:600; reliability:0.99; duty:0.01; optical:on; backscatter:on; ris:16; selector:argmax";
    ok = aurora_run(intention, engine.get());
  }

  if (!decision_trace_path.empty()) {
    try {
      engine->decision_trace_log.save(decision_trace_path);
      std::cerr << "[REPLAY] decision trace saved: " << decision_trace_path
                << " records=" << engine->decision_trace_log.records().size() << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[REPLAY] failed to save decision trace: " << error.what() << '\n';
      return 2;
    }
  }
  if (!event_ledger_path.empty()) {
    try {
      engine->simulation_event_log.save(event_ledger_path);
      std::cerr << "[REPLAY] simulation event ledger saved: "
                << event_ledger_path
                << " records=" << engine->simulation_event_log.records().size()
                << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[REPLAY] failed to save simulation event ledger: "
                << error.what() << '\n';
      return 2;
    }
  }
  if (!contact_schedule_out_path.empty()) {
    try {
      engine->contact_schedule.save(contact_schedule_out_path);
      std::cerr << "[CONTACT] schedule saved: "
                << contact_schedule_out_path
                << " windows=" << engine->contact_schedule.windows().size()
                << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[CONTACT] failed to save schedule: "
                << error.what() << '\n';
      return 2;
    }
  }
  if (!generation_arrival_schedule_out_path.empty()) {
    try {
      engine->generation_arrival_schedule.save(
        generation_arrival_schedule_out_path);
      std::cerr << "[ARRIVAL] schedule saved: "
                << generation_arrival_schedule_out_path
                << " arrivals="
                << engine->generation_arrival_schedule.arrivals().size()
                << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[ARRIVAL] failed to save schedule: "
                << error.what() << '\n';
      return 2;
    }
  }
  return ok ? 0 : 1;
}
#endif
