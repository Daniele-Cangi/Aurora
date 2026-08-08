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
#include "src/core/AuroraSafetyMonitor.hpp"
#include "include/aurora/safety/SafetyEnvelope.hpp"
#include "include/aurora/telemetry/DecisionReplayLog.hpp"
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

static aurora::safety::LinkMode safety_link(phy::Mode mode) {
  switch (mode) {
    case phy::Mode::RF: return aurora::safety::LinkMode::RF;
    case phy::Mode::IR: return aurora::safety::LinkMode::OPTICAL;
    case phy::Mode::BACKSCATTER: return aurora::safety::LinkMode::BACKSCATTER;
  }
  return aurora::safety::LinkMode::RF;
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
  string id; geom::Vec2 pos;
  energy::Store bat{10.0, 6.0};
  vector<fec::Pkt> buf, inbox; unordered_set<string> seen;
  double harvest_W = 0.2;
  size_t tx_idx = 0; // NEW: rotating cursor to avoid resending same packet

  static string packet_key(const fec::Pkt& packet) {
    return packet.generation_id + ":" + to_string(packet.segment_id) + ":" + to_string(packet.fp.seed);
  }
  void tick(double dt){ bat.harvest(harvest_W, dt); }
  void ingest(){ for(auto&p: inbox) { auto key=packet_key(p); if(!seen.count(key)){ buf.push_back(p); seen.insert(move(key));} } inbox.clear(); }
  bool send_one(world::World& W, Node& rx, phy::Mode m){
    if(buf.empty()) return false;
    if(tx_idx >= buf.size()) tx_idx = 0; // ring safety
    auto pkt = buf[tx_idx];              // rotate across distinct packets
    tx_idx = (tx_idx + 1) % max<size_t>(1, buf.size());

    size_t B = pkt.fp.data.size()+8;
    double J = phy::Jpkt(m,B); if(!bat.spend(J)) return false;
    double g  = W.multibounce_best(pos, rx.pos, 2);
    double SNR= phy::snr_db(g,m,W.illum);
    double coding_gain = (m==phy::Mode::RF? 8.0 : m==phy::Mode::IR? 3.0 : 4.0);
    bool ok = channel::pass_realistic(SNR, m, coding_gain, /*rician*/ false);
    if(ok && !rx.seen.count(packet_key(pkt))) rx.inbox.push_back(pkt);

    // size shaping safe - padding solo nel frame PHY (non parte del FEC/Merkle)
    if(m==phy::Mode::RF){
      int pad = (int)(util::rng.uni()*12);
      vector<uint8_t> frame = pkt.fp.data; frame.resize(frame.size()+pad, (uint8_t)(0xA5 ^ pad));
      HAL::LORA_TX(frame.data(), frame.size());
    } else if(m==phy::Mode::IR){
      HAL::IR_TX(pkt.fp.data.data(), pkt.fp.data.size(), 3500 + (int)(util::rng.uni()*1200));
    } else {
      vector<uint8_t> bits( pkt.fp.data.size()*8, 1 );
      HAL::BS_MODULATE(bits.data(), bits.size(), 450 + (int)(util::rng.uni()*200));
    }
    return ok;
  }
};

struct Net { world::World W; vector<unique_ptr<Node>> nodes;
  Node& add(const string& id, geom::Vec2 p){ nodes.emplace_back(make_unique<Node>()); nodes.back()->id=id; nodes.back()->pos=p; return *nodes.back(); }
  Node* get(const string& id){ for(auto& n:nodes) if(n->id==id) return n.get(); return nullptr; }
};

// ---------- Engine ----------
using FlowHealth = cl::FlowHealth;

struct Engine {
  Net net; Intention I; string token_id, bundle_id, generation_id; int K;
  size_t T=128;  // mantenuto 128 per FEC piu rapido
  size_t payload_size;
  uint32_t seqc=1;
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
  
  // T1: Flag per stream interattivo e stato corrente
  bool interactive_stream_ = false;
  aurora::safety::SafetyState current_safety_state_ = aurora::safety::SafetyState::HEALTHY;
  cl::Mode current_mode_ = cl::Mode::NORMAL;
  
  Engine() : safety_monitor(aurora::safety::SafetyConfig::default_config()) {
    // FASE 4: Inizializza organismo
    organism = make_unique<aurora::AlienFountainOrganism>();
  }

  void init(const string& intention){
    I = Intention::parse(intention);
    util::rng.reseed(I.experiment_seed);
    net.W.obs.push_back({{0.52,0.52}, 0.22});
    for(int i=0;i<I.ris_tiles; ++i){ double u=(i+1.0)/(I.ris_tiles+1.0); net.W.ris.push_back({{0.12+0.78*u, 0.12+0.78*u}, (uint8_t)(i%4)}); }
    HAL::RADIO_INIT();
    HAL::LORA_CFG(phy::EU868_CH[0], 125, 12, 5, 12);
    net.add("SRC",{0.06,0.08}); net.add("DST",{0.94,0.92});

    const uint64_t deterministic_epoch_s = 1'700'000'000ULL + (I.experiment_seed % 1'000'000ULL);
    Token t = Token::make("ACCESS:TEMP_KEY=abc123;ZONE=42;TTL=24h;CLASS=NORM;", 24*3600,
                          deterministic_epoch_s);
    Bundle b = Bundle::make(t); token_id=t.id; bundle_id=b.bid;
    auto bytes = tok2bytes(t);
    payload_size = bytes.size();
    auto spawned = organism->spawn(I, token_id, bytes, T, 0);
    generation_descriptor = spawned.descriptor;
    generation_id = generation_descriptor.generation_id;
    K = spawned.K;
    for (auto& packet : spawned.packets) {
      packet.seq = seqc++;
      net.get("SRC")->buf.push_back(std::move(packet));
    }
    cout << "[GENERATION] id=" << generation_id
         << " codec=" << generation_descriptor.codec_id
         << " K=" << K << " T=" << T
         << " emitted=" << net.get("SRC")->buf.size() << endl;
  }

  static double entropy_residual(int have, int need){ double e=max(0, need-have)/(double)need; return min(1.0,max(0.0,e)); }
  
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
    telem::ChannelState chan;
    double epoch=1.0;

    // T3: Ricarica config all'inizio se in modalità interattiva
    if (interactive_stream_) {
      reload_interactive_config("aurora_interactive_config.json");
    }

    // Iterazioni massime (sufficienti anche per deadline lunghe)
    for(int step=0; step<500 && !delivered; ++step){
      // T3: Ricarica config periodicamente (ogni 20 step)
      if (interactive_stream_ && (step % 20 == 0)) {
        reload_interactive_config("aurora_interactive_config.json");
      }
      // tick/ingest
      for(auto& n: net.nodes) n->tick(1.0), n->ingest();

      int have=static_cast<int>(last_decode_report.decoder_rank);
      double eres=entropy_residual(have,K);

      // progress
      if(step % 20 == 0 || have >= K) {
        cout << "[PROGRESS] step=" << step << " have=" << have << "/" << K 
             << " (" << fixed << setprecision(1) << (100.0*have/K) << "%)"
             << " SRC_SoC=" << setprecision(0) << (S.bat.soc()*100) << "%" << endl;
      }

      // RIS phase-field alignment
      for(auto& t: net.W.ris){
        double ang = atan2(t.p.y-S.pos.y, t.p.x-S.pos.x) + atan2(D.pos.y-t.p.y, D.pos.x-t.p.x);
        double jitter = (eres)*(util::rng.uni()-0.5)*0.9;
        double ph = fmod(ang + jitter, 2*M_PI); int idx = (int)llround(ph/(M_PI/2.0)) & 3; t.phase2b = (uint8_t)idx;
      }
      { vector<uint8_t> ph; ph.reserve(net.W.ris.size()); for(auto&t:net.W.ris) ph.push_back(t.phase2b); HAL::RIS_SET_PHASES(ph); }

      // **PATCH**: RIS illumination ramp (attiva presto, rampa 0.10->0.35)
      {
        double on = (eres - 0.10) / 0.25;  // 0.10 -> 0.35
        if (on < 0.0) on = 0.0; if (on > 1.0) on = 1.0;
        net.W.illum = on;
        if(net.W.illum > 0.0) HAL::CW_ON(0.05); else HAL::CW_OFF();
      }

      const uint64_t simulated_now_ms = static_cast<uint64_t>(step) * 1000ULL;
      const double elapsed = static_cast<double>(simulated_now_ms) / 1000.0;

      // Stato per optimizer (channel-aware + priority)
      cl::NetworkState Sx{};
      Sx.soc_src=S.bat.soc(); Sx.symbols_have=have; Sx.symbols_need=K;
      Sx.deadline_left_s=max(0.0, I.deadline_s-elapsed);
      Sx.duty_left_rf=HAL::DutyLeftHint();

      double g_probe = net.W.multibounce_best(S.pos, D.pos, 2);
      Sx.chan = chan;
      Sx.chan.push_snr(phy::Mode::RF,          phy::snr_db(g_probe, phy::Mode::RF,          net.W.illum));
      Sx.chan.push_snr(phy::Mode::BACKSCATTER, phy::snr_db(g_probe, phy::Mode::BACKSCATTER, net.W.illum));
      Sx.chan.push_snr(phy::Mode::IR,          phy::snr_db(g_probe, phy::Mode::IR,          net.W.illum));
      Sx.decode_rate_symps = (have>0? have / max(1.0, elapsed) : 0.0);

      // priority
      Sx.prio = (Sx.deadline_left_s < I.deadline_s*0.15 ? cl::Priority::CRITICAL
                : Sx.deadline_left_s < I.deadline_s*0.4 ? cl::Priority::NORMAL
                : cl::Priority::BULK);
      Sx.emergency_mode = (Sx.deadline_left_s < I.deadline_s*0.08 && (K-have) > (K*0.25));
      Sx.covert_seq = static_cast<uint16_t>((I.experiment_seed + static_cast<uint64_t>(step)) & 0xFF);

      auto dec = opt.joint(I, Sx, epoch);
      aurora::safety::TransportDecision proposed;
      proposed.link = safety_link(dec.mode);
      proposed.transmission_attempts = static_cast<uint32_t>(dec.tries & 0xFF);
      proposed.repair_symbols = static_cast<uint32_t>(dec.overhead & 0x7FFF);
      proposed.critical_only = Sx.prio == cl::Priority::CRITICAL;
      aurora::safety::TransportState observed;
      observed.observed_at_ms = simulated_now_ms;
      observed.now_ms = simulated_now_ms;
      observed.source_energy_reserve = S.bat.soc();
      observed.rf_duty_remaining = Sx.duty_left_rf;
      observed.decoder_rank = last_decode_report.decoder_rank;
      observed.required_rank = static_cast<uint32_t>(K);
      auto decision_trace = safety_envelope.constrain(I, generation_descriptor, observed, proposed);
      decision_trace_log.record(I, generation_descriptor, decision_trace);
      dec.mode = phy_link(decision_trace.decision.link);
      double hop = HAL::FHSS_next( (dec.tries>>8) & 0xFF );
      HAL::LORA_CFG(hop, dec.rf_bw_khz, 12, 5, dec.preamble_sym);

      // invio con jitter/spacing + feedback
      int tries_real = decision_trace.decision.permitted
        ? static_cast<int>(decision_trace.decision.transmission_attempts)
        : 0;
      if (decision_trace.decision.permitted && Sx.prio == cl::Priority::CRITICAL) {
        tries_real = std::max(tries_real, 2);
      }
      uint8_t covert_seq = (dec.tries >> 8) & 0xFF;
      bool emergency = (dec.overhead & 0x8000) != 0;
      int overhead_real = static_cast<int>(decision_trace.decision.repair_symbols);

      auto do_sleep = [&](int base_ms, int jitter_ms){
        if(base_ms<=0 && jitter_ms<=0) return;
        int extra = (jitter_ms>0 ? (int)(util::rng.uni()*jitter_ms) : 0);
        this_thread::sleep_for(std::chrono::milliseconds(base_ms + extra));
      };

      int ok_cnt=0; string mode_chosen;
      for(int i=0;i<tries_real; ++i){
        // invia sempre, decisione canale gia realistica in send_one()
        bool ok = S.send_one(net.W, D, dec.mode);
        if(i==0) mode_chosen = mode_to_string(dec.mode);

        // **LOG** SNR di mondo (coerente) solo per debug
        double snr_world = phy::snr_db(net.W.multibounce_best(S.pos, D.pos, 2), dec.mode, net.W.illum);
        if(step < 3) {
          std::cout << "[STEP " << step << "] mode=" << mode_chosen
                    << " SNR(world)=" << fixed << setprecision(1) << snr_world
                    << " outcome=" << (ok?"OK":"FAIL") << std::endl;
        }

        chan.push_outcome(dec.mode, ok); ok_cnt += (ok?1:0);
        do_sleep(dec.min_spacing_ms, dec.jitter_ms);
      }
      if(step < 3) std::cout << "[ADAPTIVE] step=" << step << " mode_chosen=" << mode_chosen << std::endl;

      if(decision_trace.decision.permitted && S.bat.soc()<0.25 && I.allow_backscatter){
        int extra = std::min(8, std::max(2, overhead_real/3));
        for(int k=0;k<extra; ++k){
          bool ok = S.send_one(net.W, D, phy::Mode::BACKSCATTER);
          chan.push_outcome(phy::Mode::BACKSCATTER, ok);
          do_sleep(dec.min_spacing_ms+5, dec.jitter_ms+10);
        }
      }

      double reward = clamp( (double)ok_cnt / max(1, tries_real), 0.0, 1.0 );
      if (tries_real > 0) opt.feedback(dec.mode, reward);
      if(emergency){ cout<<"[COVERT] EMERGENCY flag; seq="<<(int)covert_seq<<"\n"; }

      int have_after = 0; for (auto& p : D.buf) if (p.generation_id == generation_id) have_after++;
      
      // FASE 4: Integra organismo per ottenere risultati reali
      aurora::FlowProfile flow_profile = organism->build_profile(I);
      
      // Raccogli pacchetti ricevuti per questo token_id
      vector<fec::Pkt> received_packets;
      for (auto& p : D.buf) {
        if (p.generation_id == generation_id) {
          received_packets.push_back(p);
        }
      }
      
      auto res = organism->integrate(generation_id, received_packets, simulated_now_ms);
      last_decode_report = res;
      apply_flow_feedback(res, flow_profile.flow_class);
      if (res.delivered()) {
        delivered = true;
        out = res.payload;
        used.clear();
        for (const auto& packet : received_packets) used.push_back(packet.fp.data);
        cout << "[SUCCESS] authoritative generation decode rank="
             << res.decoder_rank << "/" << res.required_rank << endl;
      }
      
      // T1: Emetti eventi JSON health per ogni classe
      emit_health_event(step, nerve_health_, aurora::FlowClass::NERVE);
      emit_health_event(step, gland_health_, aurora::FlowClass::GLAND);
      emit_health_event(step, muscle_health_, aurora::FlowClass::MUSCLE);
      
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
      
      // FASE 4: Converti TelemetrySample locale a aurora::safety::TelemetrySample
      aurora::safety::TelemetrySample safety_sample;
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
      
      // FASE 4: Aggiorna SafetyMonitor con sample esteso
      safety_monitor.observe(safety_sample);
      
      // FASE 4: Aggiorna Optimizer mode basato su SafetyState e FlowHealth
      aurora::safety::SafetyState safety_state = safety_monitor.state();
      current_safety_state_ = safety_state;  // T1: Aggiorna stato corrente
      
      // Chiama update_mode con SafetyState e FlowHealth objects
      opt.update_mode(safety_state,
                     nerve_health_,
                     gland_health_,
                     muscle_health_);
      
      current_mode_ = opt.mode();  // T1: Aggiorna mode corrente
      
      // Log mode change (il log è già dentro update_mode, ma lo lasciamo per chiarezza)
      // Nota: update_mode() già logga quando il mode cambia
      
      telemetry.record(sample);

      if(!delivered && res.status == aurora::transport::DecodeStatus::EXPIRED){
          cout << "[TIMEOUT] Deadline exceeded at step " << step 
               << " (elapsed=" << fixed << setprecision(2) << elapsed 
               << "s, deadline=" << I.deadline_s << "s)" << endl;
          break;
      }

      epoch += 1.0;

      // **PATCH**: sleep adattivo in base al tempo rimanente
      {
        double dl_rem = std::max(0.0, I.deadline_s - elapsed);
        int ms; if      (dl_rem < 2.0) ms = 2; else if (dl_rem < 5.0) ms = 6; else ms = 12;
        this_thread::sleep_for(std::chrono::milliseconds(ms));
      }
    }

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
  telem::ChannelState chan;
  double epoch=1.0;

  // T3: Ricarica config all'inizio
  engine.reload_interactive_config("aurora_interactive_config.json");

  for(int step=0; step<max_steps && !delivered; ++step){
    
    // T3: Ricarica config periodicamente (ogni 20 step)
    if (step % 20 == 0) {
      engine.reload_interactive_config("aurora_interactive_config.json");
    }
    
    // Usa la stessa logica di run() ma in versione continua
    for(auto& n: engine.net.nodes) n->tick(1.0), n->ingest();

    int have=static_cast<int>(engine.last_decode_report.decoder_rank);
    double e=max(0, engine.K-have)/(double)engine.K; double eres=min(1.0,max(0.0,e));

    // progress (meno verboso in lab mode)
    if(step % 100 == 0 || have >= engine.K) {
      cout << "[PROGRESS] step=" << step << " have=" << have << "/" << engine.K 
           << " (" << fixed << setprecision(1) << (100.0*have/engine.K) << "%)"
           << " SRC_SoC=" << setprecision(0) << (S.bat.soc()*100) << "%" << endl;
    }

    // RIS phase-field alignment
    for(auto& t: engine.net.W.ris){
      double ang = atan2(t.p.y-S.pos.y, t.p.x-S.pos.x) + atan2(D.pos.y-t.p.y, D.pos.x-t.p.x);
      double jitter = (eres)*(util::rng.uni()-0.5)*0.9;
      double ph = fmod(ang + jitter, 2*M_PI); int idx = (int)llround(ph/(M_PI/2.0)) & 3; t.phase2b = (uint8_t)idx;
    }
    { vector<uint8_t> ph; ph.reserve(engine.net.W.ris.size()); for(auto&t:engine.net.W.ris) ph.push_back(t.phase2b); HAL::RIS_SET_PHASES(ph); }

    // RIS illumination ramp
    {
      double on = (eres - 0.10) / 0.25;
      if (on < 0.0) on = 0.0; if (on > 1.0) on = 1.0;
      engine.net.W.illum = on;
      if(engine.net.W.illum > 0.0) HAL::CW_ON(0.05); else HAL::CW_OFF();
    }

    const uint64_t simulated_now_ms = static_cast<uint64_t>(step) * 1000ULL;
    const double elapsed = static_cast<double>(simulated_now_ms) / 1000.0;

    // Stato per optimizer
    cl::NetworkState Sx{};
    Sx.soc_src=S.bat.soc(); Sx.symbols_have=have; Sx.symbols_need=engine.K;
    Sx.deadline_left_s=max(0.0, engine.I.deadline_s-elapsed);
    Sx.duty_left_rf=HAL::DutyLeftHint();

    double g_probe = engine.net.W.multibounce_best(S.pos, D.pos, 2);
    Sx.chan = chan;
    Sx.chan.push_snr(phy::Mode::RF,          phy::snr_db(g_probe, phy::Mode::RF,          engine.net.W.illum));
    Sx.chan.push_snr(phy::Mode::BACKSCATTER, phy::snr_db(g_probe, phy::Mode::BACKSCATTER, engine.net.W.illum));
    Sx.chan.push_snr(phy::Mode::IR,          phy::snr_db(g_probe, phy::Mode::IR,          engine.net.W.illum));
    Sx.decode_rate_symps = (have>0? have / max(1.0, elapsed) : 0.0);

    Sx.prio = (Sx.deadline_left_s < engine.I.deadline_s*0.15 ? cl::Priority::CRITICAL
              : Sx.deadline_left_s < engine.I.deadline_s*0.4 ? cl::Priority::NORMAL
              : cl::Priority::BULK);
    Sx.emergency_mode = (Sx.deadline_left_s < engine.I.deadline_s*0.08 && (engine.K-have) > (engine.K*0.25));
    Sx.covert_seq = static_cast<uint16_t>((engine.I.experiment_seed + static_cast<uint64_t>(step)) & 0xFF);

    auto dec = opt.joint(engine.I, Sx, epoch);
    aurora::safety::TransportDecision proposed;
    proposed.link = safety_link(dec.mode);
    proposed.transmission_attempts = static_cast<uint32_t>(dec.tries & 0xFF);
    proposed.repair_symbols = static_cast<uint32_t>(dec.overhead & 0x7FFF);
    proposed.critical_only = Sx.prio == cl::Priority::CRITICAL;
    aurora::safety::TransportState observed;
    observed.observed_at_ms = simulated_now_ms;
    observed.now_ms = simulated_now_ms;
    observed.source_energy_reserve = S.bat.soc();
    observed.rf_duty_remaining = Sx.duty_left_rf;
    observed.decoder_rank = engine.last_decode_report.decoder_rank;
    observed.required_rank = static_cast<uint32_t>(engine.K);
    auto decision_trace = engine.safety_envelope.constrain(
      engine.I, engine.generation_descriptor, observed, proposed);
    engine.decision_trace_log.record(
      engine.I, engine.generation_descriptor, decision_trace);
    dec.mode = phy_link(decision_trace.decision.link);
    double hop = HAL::FHSS_next( (dec.tries>>8) & 0xFF );
    HAL::LORA_CFG(hop, dec.rf_bw_khz, 12, 5, dec.preamble_sym);

    int tries_real = decision_trace.decision.permitted
      ? static_cast<int>(decision_trace.decision.transmission_attempts)
      : 0;
    if (decision_trace.decision.permitted && Sx.prio == cl::Priority::CRITICAL) {
      tries_real = std::max(tries_real, 2);
    }
    uint8_t covert_seq = (dec.tries >> 8) & 0xFF;
    bool emergency = (dec.overhead & 0x8000) != 0;
    int overhead_real = static_cast<int>(decision_trace.decision.repair_symbols);

    auto do_sleep = [&](int base_ms, int jitter_ms){
      if(base_ms<=0 && jitter_ms<=0) return;
      int extra = (jitter_ms>0 ? (int)(util::rng.uni()*jitter_ms) : 0);
      this_thread::sleep_for(std::chrono::milliseconds(base_ms + extra));
    };

    int ok_cnt=0; string mode_chosen;
    for(int i=0;i<tries_real; ++i){
      bool ok = S.send_one(engine.net.W, D, dec.mode);
      if(i==0) mode_chosen = mode_to_string(dec.mode);
      chan.push_outcome(dec.mode, ok); ok_cnt += (ok?1:0);
      do_sleep(dec.min_spacing_ms, dec.jitter_ms);
    }

    if(decision_trace.decision.permitted && S.bat.soc()<0.25 && engine.I.allow_backscatter){
      int extra = std::min(8, std::max(2, overhead_real/3));
      for(int k=0;k<extra; ++k){
        bool ok = S.send_one(engine.net.W, D, phy::Mode::BACKSCATTER);
        chan.push_outcome(phy::Mode::BACKSCATTER, ok);
        do_sleep(dec.min_spacing_ms+5, dec.jitter_ms+10);
      }
    }

    double reward = clamp( (double)ok_cnt / max(1, tries_real), 0.0, 1.0 );
    if (tries_real > 0) opt.feedback(dec.mode, reward);
    if(emergency){ cout<<"[COVERT] EMERGENCY flag; seq="<<(int)covert_seq<<"\n"; }

    int have_after = 0; for (auto& p : D.buf) if (p.generation_id == engine.generation_id) have_after++;
    
    // Integra organismo
    aurora::FlowProfile flow_profile = engine.organism->build_profile(engine.I);
    
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
    
    // T1: Emetti eventi JSON health (DOPO apply_flow_feedback per avere dati aggiornati)
    engine.emit_health_event(step, engine.nerve_health_, aurora::FlowClass::NERVE);
    engine.emit_health_event(step, engine.gland_health_, aurora::FlowClass::GLAND);
    engine.emit_health_event(step, engine.muscle_health_, aurora::FlowClass::MUSCLE);
    
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
    
    aurora::safety::TelemetrySample safety_sample;
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
    
    engine.safety_monitor.observe(safety_sample);
    
    aurora::safety::SafetyState safety_state = engine.safety_monitor.state();
    engine.current_safety_state_ = safety_state;
    
    opt.update_mode(safety_state,
                   engine.nerve_health_,
                   engine.gland_health_,
                   engine.muscle_health_);
    
    engine.current_mode_ = opt.mode();
    
    engine.telemetry.record(sample);

    if (!delivered && res.status == aurora::transport::DecodeStatus::EXPIRED) {
      break;
    }

    epoch += 1.0;

    // Sleep adattivo
    {
      double dl_rem = std::max(0.0, engine.I.deadline_s - elapsed);
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
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--interactive-lab") {
      interactive_lab = true;
    } else if (std::string(argv[i]) == "--decision-trace") {
      if (i + 1 >= argc) {
        std::cerr << "--decision-trace requires a file path\n";
        return 2;
      }
      decision_trace_path = argv[++i];
    }
  }

  auto engine = std::make_unique<Engine>();
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
  return ok ? 0 : 1;
}
#endif
