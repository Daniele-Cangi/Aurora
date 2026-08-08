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
  struct Window { deque<double> q; int N; explicit Window(int n=30):N(n){} void push(double x){ q.push_back(x); if((int)q.size()>N) q.pop_back(); } double mean()const{ if(q.empty()) return NAN; double s=0; for(double x:q)s+=x; return s/q.size(); } double var()const{ if((int)q.size()<2) return NAN; double m=mean(), s=0; for(double x:q)s+=(x-m)*(x-m); return s/(q.size()-1); } };
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
  static inline bool pass_realistic(double snr_db, phy::Mode m, double coding_gain_db, bool rician=false){
    double eff = snr_db + coding_gain_db + fading_db(rician);
    return eff > threshold(m);
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

  struct UCB {
    array<int,3> N{0,0,0};
    array<double,3> reward{0,0,0};
    int choose(double t){
      double C = 1.2; double best=-1e9; int idx=0;
      for(int i=0;i<3;++i){
        double avg = (N[i]>0? reward[i]/N[i] : 0.7);
        double conf = (N[i]>0? C * sqrt(log(max(1.0,t))/N[i]) : 1.0);
        double score = avg + conf;
        if(score>best){ best=score; idx=i; }
      }
      return idx;
    }
    void update(int idx, double r){ N[idx]++; reward[idx]+=r; }
  };

  using FlowHealth = aurora::transport::TransportHealth;

  // FASE 4: Mode enum per Optimizer
  enum class Mode {
    CONSERVATIVE,
    NORMAL,
    AGGRESSIVE
  };

  struct Optimizer{
    UCB bandit;
    // Hysteresis state for ARGMAX selector
    phy::Mode last_mode = phy::Mode::RF;
    double hysteresis_dB = 1.0;
    
    // FASE 4: Operating mode
    Mode mode_ = Mode::NORMAL;
    
    static int midx(phy::Mode m){ return m==phy::Mode::RF?0: (m==phy::Mode::IR?1:2); }
    static phy::Mode imode(int i){ return i==0?phy::Mode::RF: (i==1?phy::Mode::IR:phy::Mode::BACKSCATTER); }

  #include "aurora_intention.hpp"
  Decision joint(const Intention& I, const NetworkState& S, double epoch){
      phy::Mode m = phy::Mode::RF;
      if(I.selector_argmax){
        // ARGMAX SNR + hysteresis
        double snr_rf  = S.chan.snr_est(phy::Mode::RF);
        double snr_ir  = S.chan.snr_est(phy::Mode::IR);
        double snr_bs  = S.chan.snr_est(phy::Mode::BACKSCATTER);
        phy::Mode best = phy::Mode::RF; double best_snr = snr_rf;
        if(snr_ir  > best_snr){ best=phy::Mode::IR; best_snr=snr_ir; }
        if(snr_bs  > best_snr){ best=phy::Mode::BACKSCATTER; best_snr=snr_bs; }
        double prev_snr = S.chan.snr_est(last_mode);
        double margin   = best_snr - prev_snr;
        m = (margin > hysteresis_dB ? best : last_mode);
      } else {
        // UCB bandit
        int choice = bandit.choose(max(1.0, epoch));
        m = imode(choice);
      }
      if(S.soc_src<0.18) m = (I.allow_backscatter? phy::Mode::BACKSCATTER : m);
      last_mode = m;

      double R = target_R_for(S.prio);
      if(S.emergency_mode) R = max(R, 0.999);

      // FASE 4: Modifica R, budget, cap in base a mode_
      if (mode_ == Mode::CONSERVATIVE) {
        // CONSERVATIVE: proteggere NERVE/GLAND, limitare esperimenti/bulk
        // Aumenta R per flussi critici, riduci cap per limitare bulk
        if (S.prio == Priority::CRITICAL || S.prio == Priority::NORMAL) {
          R = max(R, 0.995);  // R più alto per flussi critici
        }
      } else if (mode_ == Mode::AGGRESSIVE) {
        // AGGRESSIVE: più esperimenti su MUSCLE se tutto è stabile
        // Riduci R leggermente per MUSCLE, aumenta cap per più tentativi
        if (S.prio == Priority::BULK) {
          R = max(0.85, R - 0.05);  // R leggermente più basso per bulk
        }
      }
      // NORMAL: comportamento standard (nessuna modifica)

      double urg = urgency(S.symbols_have, S.symbols_need, I.deadline_s, S.deadline_left_s);
      double budget = allocate_duty_budget(S.duty_left_rf, urg);
      
      // FASE 4: Modifica cap in base a mode_
      int cap_base = (budget>0.5? 48 : budget>0.25? 32 : 20);
      int cap = cap_base;
      if (mode_ == Mode::CONSERVATIVE) {
        cap = max(12, cap_base - 8);  // Riduci cap per limitare bulk
      } else if (mode_ == Mode::AGGRESSIVE) {
        cap = min(64, cap_base + 8);  // Aumenta cap per più tentativi
      }

      double per = per_est(S, m);
      double ps  = 1.0 - per;
      int tries = attempts_for_R(R, ps, cap);

      int redundancy = (int)ceil( log(1.0 - R) / log(per) * 0.6 );
      redundancy = max(5, redundancy);
      
      // FASE 4: Modifica redundancy in base a mode_
      if (mode_ == Mode::CONSERVATIVE && (S.prio == Priority::CRITICAL || S.prio == Priority::NORMAL)) {
        redundancy = (int)(redundancy * 1.2);  // Aumenta ridondanza per flussi critici
      } else if (mode_ == Mode::AGGRESSIVE && S.prio == Priority::BULK) {
        redundancy = max(3, (int)(redundancy * 0.9));  // Riduci leggermente per bulk
      }

      int base_space = (S.soc_src<0.3 ? 18 : 8);
      int jitter = (int)round((1.0 - clamp(S.duty_left_rf,0.0,1.0))*40.0) + (S.soc_src<0.3?12:0);
      int preamble = (int)clamp(8 + (int)(urg*10) + (int)(util::rng.uni()*4), 6, 24);
      int bw = (util::rng.uni()<0.5 ? 125 : 250);
      
      // FASE 4: CONSERVATIVE: favorisci modi radio più robusti per NERVE/GLAND
      if (mode_ == Mode::CONSERVATIVE && (S.prio == Priority::CRITICAL || S.prio == Priority::NORMAL)) {
        // Se siamo in CONSERVATIVE e il flusso è critico, preferisci RF (più robusto)
        // ma solo se non è già stato scelto un modo migliore
        double snr_rf = S.chan.snr_est(phy::Mode::RF);
        double snr_ir = S.chan.snr_est(phy::Mode::IR);
        if (snr_rf > snr_ir - 2.0) {  // Se RF è comparabile (entro 2dB), preferisci RF
          m = phy::Mode::RF;
        }
      }

      int overhead = redundancy & 0x7FFF;
      if(S.emergency_mode) overhead |= 0x8000;
      tries = (tries & 0xFF) | ((S.covert_seq & 0xFF) << 8);

      return { m, tries, overhead, jitter, base_space, preamble, bw };
    }

    void feedback(phy::Mode m, double reward_val){ bandit.update(midx(m), clamp(reward_val, 0.0, 1.0)); }
    
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
        // Convert SafetyState to int (assumes it can be cast to int: 0=HEALTHY, 1=DEGRADED, 2=CRITICAL)
        int safety_state_int = static_cast<int>(safety_state);
        double nerve_fail_rate = nerve_health.ewma_fail_rate;
        double gland_fail_rate = gland_health.ewma_fail_rate;
        double nerve_cov = nerve_health.ewma_coverage;
        double gland_cov = gland_health.ewma_coverage;
        (void)muscle_health;
        
        Mode old_mode = mode_;
        
        // Converti safety_state_int a logica
        if (safety_state_int == 2) {  // CRITICAL
            mode_ = Mode::CONSERVATIVE;
        } else if (safety_state_int == 1) {  // DEGRADED
            mode_ = Mode::NORMAL;
        } else {  // HEALTHY
            // Guarda anche lo stato dei flussi
            if (nerve_fail_rate < 0.05 &&
                gland_fail_rate < 0.05 &&
                nerve_cov > 0.95 &&
                gland_cov > 0.95) {
                mode_ = Mode::AGGRESSIVE;
            } else {
                mode_ = Mode::NORMAL;
            }
        }
        
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


