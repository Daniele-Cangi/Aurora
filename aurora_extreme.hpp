// aurora_extreme.hpp
#pragma once
// Fix for M_PI and math constants
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <iostream>
#include <vector>
#include <string>
#include <array>
#include <memory>
#include <unordered_set>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <map>
#include <functional>
#include <deque>
#include <random>
using namespace std;

#include "aurora_hal.hpp"
#include "include/aurora/control/CrossLayerProposal.hpp"
#include "include/aurora/control/OperatingModeController.hpp"
#include "include/aurora/fec/LtLikeCodec.hpp"
#include "include/aurora/transport/TransportHealth.hpp"
#ifdef _WIN32
#undef byte
#endif

// ===== CRYPTO (Ed25519) =====
namespace CRYPTO {
#ifdef AURORA_USE_REAL_CRYPTO
  #include <sodium.h>
  inline void ensure(){ static bool inited=false; if(!inited){ if(sodium_init()<0) abort(); inited=true; } }
  inline void ed25519_keypair(uint8_t pk[32], uint8_t sk[64]){ ensure(); crypto_sign_ed25519_keypair(pk, sk); }
  inline void ed25519_sign(uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t sk[64]){ ensure(); crypto_sign_ed25519_detached(sig,nullptr,m,n,sk); }
  inline bool ed25519_verify(const uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t pk[32]){ ensure(); return crypto_sign_ed25519_verify_detached(sig,m,n,pk)==0; }
  inline string b64(const uint8_t* p,size_t n){ static const char* A="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; string o; uint32_t v=0; int vb=-6; for(size_t i=0;i<n;++i){ v=(v<<8)+p[i]; vb+=8; while(vb>=0){ o.push_back(A[(v>>vb)&0x3F]); vb-=6; } } if(vb>-6) o.push_back(A[((v<<8)>>(vb+8))&0x3F]); while(o.size()%4) o.push_back('='); return o; }
#else
  inline void ed25519_keypair(uint8_t pk[32], uint8_t sk[64]){ memset(pk,0xAA,32); memset(sk,0xBB,64); }
  inline void ed25519_sign(uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t sk[64]){ string s((const char*)m,n); string t=util::h64(string((const char*)sk,64)+s); memcpy(sig,t.data(), min((size_t)64,t.size())); }
  inline bool ed25519_verify(const uint8_t sig[64], const uint8_t* m, size_t n, const uint8_t pk[32]){ (void)pk; string s((const char*)m,n); string t=util::h64(string(64,'\xBB')+s); return memcmp(sig,t.data(), min((size_t)64,t.size()))==0; }
  inline string b64(const uint8_t* p,size_t n){ static const char*A="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; string o; uint32_t v=0; int vb=-6; for(size_t i=0;i<n;++i){ v=(v<<8)+p[i]; vb+=8; while(vb>=0){ o.push_back(A[(v>>vb)&0x3F]); vb-=6;} } if(vb>-6) o.push_back(A[((v<<8)>>(vb+8))&0x3F]); while(o.size()%4) o.push_back('='); return o; }
#endif
}

// ===== PoD-Merkle =====
namespace podm {
  static inline string leaf(const vector<uint8_t>& v){ return util::h64(string((const char*)v.data(), v.size())); }
  static inline string h2(const string&a,const string&b){ return util::h64(a+b); }
  static inline string root(vector<string> L){ if(L.empty()) return util::h64("EMPTY"); while(L.size()>1){ vector<string> n; for(size_t i=0;i<L.size(); i+=2){ string A=L[i], B=(i+1<L.size()?L[i+1]:L[i]); n.push_back(h2(A,B)); } L.swap(n);} return L[0]; }
}

// ===== Channel telemetry & estimators (rolling) — PATCH ⬇︎
namespace telem {
  struct EWMA { double a, v; EWMA(double alpha=0.2):a(alpha),v(NAN){} void push(double x){ v = std::isnan(v)? x : (a*x + (1-a)*v); } double val()const{return v;} };
  struct Window { deque<double> q; int N; explicit Window(int n=30):N(n){} void push(double x){ q.push_back(x); if((int)q.size()>N) q.pop_front(); } double mean()const{ if(q.empty()) return NAN; double s=0; for(double x:q)s+=x; return s/q.size(); } double var()const{ if((int)q.size()<2) return NAN; double m=mean(), s=0; for(double x:q)s+=(x-m)*(x-m); return s/(q.size()-1); } };
  struct ChannelState {
    EWMA snr_rf{0.25}, snr_ir{0.25}, snr_bs{0.25};
    Window per_rf{50}, per_ir{50}, per_bs{50};
    EWMA lbt_fail_rate{0.2};
    EWMA cw_interf_level{0.2};
    double coherence_s = 1.0;
    double jamming_score = 0.0;
    void push_snr(phy::Mode m, double snr){ if(m==phy::Mode::RF) snr_rf.push(snr); else if(m==phy::Mode::IR) snr_ir.push(snr); else snr_bs.push(snr); }
    void push_outcome(phy::Mode m, bool ok){ double per = ok? 0.0 : 1.0; (m==phy::Mode::RF? per_rf : m==phy::Mode::IR? per_ir : per_bs).push(per); }
    double per_est(phy::Mode m) const { auto M = (m==phy::Mode::RF? per_rf.mean() : m==phy::Mode::IR? per_ir.mean() : per_bs.mean()); return std::isnan(M)? 0.4 : std::clamp(M, 0.01, 0.99); }
    double snr_est(phy::Mode m) const { double s = (m==phy::Mode::RF? snr_rf.val() : m==phy::Mode::IR? snr_ir.val() : snr_bs.val()); return std::isnan(s)? -3.0 : s; }
  };
}

// ===== Channel realistic =====
namespace channel {
  static inline double randn(){ double u1=max(1e-12, util::rng.uni()), u2=max(1e-12, util::rng.uni()); return sqrt(-2.0*log(u1))*cos(2*M_PI*u2); }
  static inline double fading_db(bool rician=false, double KdB=6.0){
    if(!rician) return randn()*3.5 - 5.0;
    return randn()*2.5 - 2.0 + 0.2*KdB;
  }
  static inline double threshold(phy::Mode m){
    switch(m){ case phy::Mode::RF: return -9.0; case phy::Mode::IR: return 2.0; case phy::Mode::BACKSCATTER: return 0.0; }
    return 0.0;
  }
  static inline double per_from_snr(double snr_db, phy::Mode m){
    auto logistic = [](double x, double x0, double k){ return 1.0/(1.0+std::exp(k*(x-x0))); };
    switch(m){
      case phy::Mode::RF:         return logistic(snr_db, -7.5, 0.9);
      case phy::Mode::IR:         return logistic(snr_db,  4.0, 1.1);
      case phy::Mode::BACKSCATTER:return logistic(snr_db,  1.5, 1.0);
    }
    return 0.5;
  }
  struct OutcomeTrace {
    double snr_db=0.0;
    double coding_gain_db=0.0;
    double fading_db=0.0;
    double threshold_db=0.0;
    bool delivered=false;
  };
  static inline OutcomeTrace pass_realistic_trace(double snr_db, phy::Mode m, double coding_gain_db, bool rician=false){
    OutcomeTrace result;
    result.snr_db=snr_db;
    result.coding_gain_db=coding_gain_db;
    result.fading_db=fading_db(rician);
    result.threshold_db=threshold(m);
    result.delivered=result.snr_db + result.coding_gain_db + result.fading_db > result.threshold_db;
    return result;
  }
  static inline bool pass_realistic(double snr_db, phy::Mode m, double coding_gain_db, bool rician=false){
    return pass_realistic_trace(snr_db,m,coding_gain_db,rician).delivered;
  }
}

// ===== Cross-layer Optimizer (EXTREME DISTRIBUTED) — PATCH ⬇︎
namespace cl {

  enum class Priority { CRITICAL, NORMAL, BULK };
  inline double target_R_for(Priority p){
    switch(p){ case Priority::CRITICAL: return 0.999; case Priority::NORMAL: return 0.97; case Priority::BULK: default: return 0.9; }
  }

  struct NetworkState{
    double soc_src=1.0, soc_relay=1.0, soc_dst=1.0;
    double duty_left_rf=1.0;
    int    symbols_have=0, symbols_need=1;
    double deadline_left_s=600.0;
    telem::ChannelState chan;
    double decode_rate_symps = 0.0;
    bool   congestion = false;
    bool     emergency_mode=false;
    uint16_t covert_seq=0;
    Priority prio = Priority::NORMAL;
  };

  struct Decision{
    phy::Mode mode;
    int   tries;          // low8 reali, up8 covert_seq
    int   overhead;       // low15 reali, MSB emergency
    int   jitter_ms;
    int   min_spacing_ms;
    int   preamble_sym;
    int   rf_bw_khz;
  };

  inline double urgency(int have,int need,double dl_total,double dl_left){
    need = max(need,1);
    double ti = 1.0 - clamp(dl_left/max(1.0,dl_total),0.0,1.0);
    double time_press = 1.0 - exp(-6.0*ti);
    double frac = (need - have)/(double)need;
    double sym_press  = 1.0 / (1.0 + exp(-10.0*(frac-0.5)));
    return max(time_press, sym_press);
  }
  inline double allocate_duty_budget(double duty_left, double urg){
    double spend = min(0.6*duty_left, 0.1 + 0.7*urg*duty_left);
    return clamp(spend, 0.02, max(0.02, duty_left));
  }
  inline double per_est(const NetworkState& S, phy::Mode m){
    double p_hist = S.chan.per_est(m);
    double s = S.chan.snr_est(m);
    double p_snr  = channel::per_from_snr(s, m);
    double w = clamp(0.5 + 0.4*S.chan.jamming_score, 0.1, 0.9);
    return clamp(w*p_hist + (1.0-w)*p_snr, 0.01, 0.99);
  }
  inline int attempts_for_R(double R, double ps, int cap){
    ps = clamp(ps, 1e-3, 0.999);
    int n = (int)ceil( log(1.0 - R) / log(1.0 - ps) );
    return clamp(n, 1, cap);
  }

  using FlowHealth = aurora::transport::TransportHealth;

  // FASE 4: Mode enum per Optimizer
  using Mode = aurora::control::OperatingMode;

  struct Optimizer{
    aurora::control::ProposalStateSnapshot proposal_state_;
    
    // FASE 4: Operating mode
    Mode mode_ = Mode::NORMAL;
    
    static aurora::control::ProposalPriority proposal_priority(Priority priority) {
      switch (priority) {
        case Priority::CRITICAL: return aurora::control::ProposalPriority::CRITICAL;
        case Priority::NORMAL: return aurora::control::ProposalPriority::NORMAL;
        case Priority::BULK: return aurora::control::ProposalPriority::BULK;
      }
      throw invalid_argument("optimizer: invalid priority");
    }

    aurora::control::ProposalInput proposal_input(
        const Intention& intention,
        const NetworkState& state,
        double epoch,
        bool has_critical_segments) const {
      aurora::control::ProposalInput input;
      input.selector_argmax = intention.selector_argmax;
      input.allow_backscatter = intention.allow_backscatter;
      input.deadline_s = intention.deadline_s;
      input.source_soc = state.soc_src;
      input.rf_duty_remaining = state.duty_left_rf;
      input.symbols_have = state.symbols_have;
      input.symbols_need = state.symbols_need;
      input.deadline_left_s = state.deadline_left_s;
      input.snr_db = {
        state.chan.snr_est(phy::Mode::RF),
        state.chan.snr_est(phy::Mode::IR),
        state.chan.snr_est(phy::Mode::BACKSCATTER)};
      input.historical_per = {
        state.chan.per_est(phy::Mode::RF),
        state.chan.per_est(phy::Mode::IR),
        state.chan.per_est(phy::Mode::BACKSCATTER)};
      input.jamming_score = state.chan.jamming_score;
      input.priority = proposal_priority(state.prio);
      input.emergency_mode = state.emergency_mode;
      input.covert_sequence = state.covert_seq;
      input.operating_mode = mode_;
      input.epoch = epoch;
      input.has_critical_segments = has_critical_segments;
      return input;
    }

    aurora::control::ProposalDecision propose(
        const aurora::control::ProposalInput& input) {
      return aurora::control::derive_proposal(input, proposal_state_);
    }

    Decision joint(const Intention& intention,
                   const NetworkState& state,
                   double epoch) {
      const auto proposal = propose(
        proposal_input(intention, state, epoch, false));
      const auto mode = proposal.transport.link == aurora::safety::LinkMode::RF
        ? phy::Mode::RF
        : proposal.transport.link == aurora::safety::LinkMode::OPTICAL
          ? phy::Mode::IR
          : phy::Mode::BACKSCATTER;
      int tries = static_cast<int>(proposal.transport.transmission_attempts) |
        (static_cast<int>(proposal.covert_sequence) << 8);
      int overhead = static_cast<int>(proposal.transport.repair_symbols);
      if (proposal.emergency) overhead |= 0x8000;
      return {
        mode,
        tries,
        overhead,
        proposal.jitter_ms,
        proposal.minimum_spacing_ms,
        proposal.preamble_symbols,
        proposal.rf_bandwidth_khz};
    }

    void apply_feedback(const aurora::control::ProposalFeedback& feedback) {
      aurora::control::apply_proposal_feedback(proposal_state_, feedback);
    }

    void feedback(phy::Mode link, double reward) {
      const auto replay_link = link == phy::Mode::RF
        ? aurora::safety::LinkMode::RF
        : link == phy::Mode::IR
          ? aurora::safety::LinkMode::OPTICAL
          : aurora::safety::LinkMode::BACKSCATTER;
      apply_feedback({true, replay_link, clamp(reward, 0.0, 1.0)});
    }

    void reseed_proposal(uint64_t seed) {
      proposal_state_.random_state = seed ? seed : 0xC0FFEEBEEFULL;
    }

    const aurora::control::ProposalStateSnapshot& proposal_state() const {
      return proposal_state_;
    }
    
    // FASE 4: Mode getter/setter
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }
    
    // FASE 4: Update mode based on SafetyState and FlowHealth metrics
    // Forward declaration per SafetyState (definito in AuroraSafetyMonitor.hpp)
    template<typename SafetyStateType>
    void update_mode(SafetyStateType safety_state,  // SafetyState enum
                     const FlowHealth& nerve_health,
                     const FlowHealth& gland_health,
                     const FlowHealth& muscle_health) {
        // SafetyState preserves 0=HEALTHY, 1=DEGRADED, 2=CRITICAL and uses
        // -1 for NO_EVIDENCE. Missing evidence cannot authorize exploration.
        aurora::control::OperatingModeInput input;
        input.safety_state = safety_state;
        input.nerve_fail_rate = nerve_health.ewma_fail_rate;
        input.gland_fail_rate = gland_health.ewma_fail_rate;
        input.nerve_coverage = nerve_health.ewma_coverage;
        input.gland_coverage = gland_health.ewma_coverage;
        (void)muscle_health;
        
        Mode old_mode = mode_;
        mode_ = aurora::control::select_operating_mode(input);
        
        // Log solo se il modo è cambiato
        if (old_mode != mode_) {
            std::string mode_str = (mode_ == Mode::CONSERVATIVE ? "CONSERVATIVE" :
                                   mode_ == Mode::NORMAL ? "NORMAL" : "AGGRESSIVE");
            std::cout << "[OPT][MODE] " << mode_str << std::endl;
        }
    }
  };

  // T3: AuroraInteractiveConfig per parametri regolabili da file
  struct AuroraInteractiveConfig {
    double alpha_up = 0.10;        // reazione immunitaria verso l'alto
    double alpha_down = 0.02;      // dimagrimento
    int panic_boost_steps = 3;     // durata adrenalina
    double success_prob_nerve = 0.95;
    double success_prob_gland = 0.95;
    double success_prob_muscle = 0.95;
  };

  inline AuroraInteractiveConfig& get_interactive_config() {
    static AuroraInteractiveConfig cfg;
    return cfg;
  }

} // namespace cl


