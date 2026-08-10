// aurora_hal.hpp
#pragma once
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
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <map>
#include <functional>
#include <deque>
#include <random>
#include <chrono>
#include <cstdint>
#include <thread>
using namespace std;
using namespace chrono;

namespace util {
  struct RNG{ uint64_t s; explicit RNG(uint64_t x=0xC0FFEEBEEFULL):s(x){} inline void reseed(uint64_t x){ s=x?x:0xC0FFEEBEEFULL; } inline uint64_t next(){ s^=s<<7; s^=s>>9; s^=s<<8; return s; } inline double uni(){ return (next()>>11) * (1.0/((1ull<<53)-1)); } };
  static inline RNG rng;
  static inline uint64_t now_s(){ return (uint64_t)time(nullptr); }
  static inline string h64(const string& s){ uint64_t z=0x9E3779B97F4A7C15ULL; for(unsigned char c:s){ z^=(uint64_t)c+0x9e3779b97f4a7c15ULL+(z<<6)+(z>>2);} z^=z<<13; z^=z>>7; z^=z<<17; stringstream ss; ss<<hex<<setw(16)<<setfill('0')<<z; return ss.str(); }
}

namespace HAL {
// Duty + LBT
struct DutyLimiter {
  double window_s, max_frac; time_point<steady_clock> t0=steady_clock::now(); double used_s=0;
  DutyLimiter(double w, double f):window_s(w),max_frac(f){}
  bool allow(double burst_s){
    auto now=steady_clock::now(); double dt=duration<double>(now-t0).count();
    if(dt>window_s){ t0=now; used_s=0; }
    if(used_s + burst_s <= max_frac*window_s){ used_s+=burst_s; return true; }
    return false;
  }
  double left_frac() const {
    double cap=max_frac*window_s; return cap>0? max(0.0, (cap-used_s)/cap):0.0;
  }
};

// Deterministic fixed-window accounting for the simulator. It is deliberately
// separate from DutyLimiter's steady_clock accounting used by real-time HAL calls.
struct SimulationDutyLimiter {
  uint64_t window_ms = 60'000;
  double max_frac = 0.01;
  uint64_t window_start_ms = 0;
  double used_s = 0.0;

  void configure(double fraction, uint64_t window=60'000) {
    max_frac = clamp(fraction, 0.0, 1.0);
    window_ms = max<uint64_t>(1, window);
    window_start_ms = 0;
    used_s = 0.0;
  }
  void advance(uint64_t now_ms) {
    if(now_ms < window_start_ms || now_ms - window_start_ms >= window_ms) {
      window_start_ms = now_ms;
      used_s = 0.0;
    }
  }
  double capacity_s() const { return max_frac * (static_cast<double>(window_ms) / 1000.0); }
  double remaining_s(uint64_t now_ms) {
    advance(now_ms);
    return max(0.0, capacity_s() - used_s);
  }
  double remaining_fraction(uint64_t now_ms) {
    const double cap = capacity_s();
    return cap > 0.0 ? remaining_s(now_ms) / cap : 0.0;
  }
  bool can_allow(uint64_t now_ms, double burst_s) {
    return isfinite(burst_s) && burst_s >= 0.0 && burst_s <= remaining_s(now_ms) + 1e-12;
  }
  bool consume(uint64_t now_ms, double burst_s) {
    if(!can_allow(now_ms, burst_s)) return false;
    used_s += burst_s;
    return true;
  }
};
struct LBTResult {
  bool accepted=false;
  int threshold_dbm=-95;
  int first_rssi_dbm=0;
  bool second_valid=false;
  int second_rssi_dbm=0;
};
struct LBT {
  double thresh_dBm=-95;
  int dwell_ms=5;
  LBTResult clear_trace(function<int(void)> rssi, bool wall_clock_pacing=true){
    LBTResult result;
    result.threshold_dbm=static_cast<int>(thresh_dBm);
    result.first_rssi_dbm=rssi();
    if(result.first_rssi_dbm<thresh_dBm){
      if(wall_clock_pacing) this_thread::sleep_for(milliseconds(dwell_ms));
      result.second_valid=true;
      result.second_rssi_dbm=rssi();
      result.accepted=result.second_rssi_dbm<thresh_dBm;
    }
    return result;
  }
  bool clear(function<int(void)> rssi){ return clear_trace(move(rssi)).accepted; }
};

struct SimulationTransmitTrace {
  bool accepted=false;
  bool replayable=true;
  LBTResult lbt;
};

// Backend
#ifdef FIELD_BUILD
struct SPI{ bool init(int=0,int=0,int=8000000){ return true; } bool xfer(const uint8_t*,uint8_t*,size_t){ return true; } };
struct I2C{ bool init(int=1,int=400000){ return true; } bool write(uint8_t,const uint8_t*,size_t){ return true; } };
struct GPIO{ static void mode(int,bool){} static void write(int,bool){} static int read(int){ return -100; } };
#else
struct SPI{ bool init(int=0,int=0,int=8000000){ return true; } bool xfer(const uint8_t*,uint8_t*,size_t){ return true; } };
struct I2C{ bool init(int=1,int=400000){ return true; } bool write(uint8_t,const uint8_t*,size_t){ return true; } };
struct GPIO{ static void mode(int,bool){} static void write(int,bool){} static int read(int){ return -100; } };
#endif

// SX1262 driver (mock robusto; mappa a reale in FIELD_BUILD)
struct SX1262 {
  SPI spi; int pinCS=0, pinBUSY=1, pinRESET=2, pinDIO1=3;
  DutyLimiter duty{60.0, 0.01}; LBT lbt;
  bool init(){ spi.init(); return true; }
  bool cfg(double /*f*/, int /*bw_khz*/, int /*sf*/, int /*cr*/, int /*preamble_sym*/){ return true; }
  int  rssi(){ return -110 + (int)(util::rng.uni()*20); }
  bool tx(const uint8_t*, size_t len){
    double t_s=max(0.005, len / (125000.0/8.0));
    if(!duty.allow(t_s)) return false;
    if(!lbt.clear([&](){return rssi();})) return false;
    this_thread::sleep_for(milliseconds((int)(t_s*1000)));
    return true;
  }
  SimulationTransmitTrace tx_simulation_trace(const uint8_t*, size_t len){
    SimulationTransmitTrace result;
    double t_s=max(0.005, len / (125000.0/8.0));
#ifdef FIELD_BUILD
    constexpr bool wall_clock_pacing=true;
#else
    constexpr bool wall_clock_pacing=false;
#endif
    result.lbt=lbt.clear_trace([&](){return rssi();}, wall_clock_pacing);
    result.accepted=result.lbt.accepted;
    if(!result.accepted) return result;
    if(wall_clock_pacing){
      this_thread::sleep_for(milliseconds((int)(t_s*1000)));
    }
    return result;
  }
  bool tx_simulation(const uint8_t* data, size_t len){ return tx_simulation_trace(data,len).accepted; }
  bool tx_cw(double secs){
    if(!duty.allow(min(1.0,secs))) return false;
    this_thread::sleep_for(milliseconds((int)(secs*1000)));
    return true;
  }
  double duty_left() const { return duty.left_frac(); }
};

// IR / Backscatter / RIS
struct IR { bool init(){ return true; } bool tx(const uint8_t*, size_t len, int bitrate){ this_thread::sleep_for(milliseconds((int)max(1.0, len*1000.0/bitrate))); return true; } };
struct BS { bool init(){ return true; } bool mod(const uint8_t*, size_t nbits, int bitrate){ this_thread::sleep_for(milliseconds((int)max(1.0, nbits*1000.0/bitrate))); return true; } };
struct RIS { I2C i2c; uint8_t addr=0x20; int cells=16; bool init(){ i2c.init(); return true; } bool set(const vector<uint8_t>& p){
  vector<uint8_t> reg; reg.reserve((cells+3)/4);
  uint8_t acc=0; int cnt=0;
  for(int i=0;i<cells; ++i){ uint8_t two=(i<(int)p.size()? p[i]&0x3:0); acc |= (two << (2*(i%4))); if(++cnt==4){ reg.push_back(acc); acc=0; cnt=0; } }
  if(cnt) reg.push_back(acc);
  return i2c.write(addr, reg.data(), reg.size());
} };

// Facade
static SX1262& radio(){ static SX1262 r; static bool inited=false; if(!inited){ r.init(); inited=true;} return r; }
static IR& ir(){ static IR x; static bool inited=false; if(!inited){ x.init(); inited=true;} return x; }
static BS& bs(){ static BS x; static bool inited=false; if(!inited){ x.init(); inited=true;} return x; }
static RIS& ris(){ static RIS x; static bool inited=false; if(!inited){ x.init(); inited=true;} return x; }

// API — PATCH ⬇︎ (preamble & BW dinamici)
inline bool RADIO_INIT(){ radio().init(); return true; }
inline bool LORA_CFG(double f,int bw_khz,int sf,int cr,int preamble_sym){ (void)f;(void)bw_khz;(void)sf;(void)cr;(void)preamble_sym; return radio().cfg(f,bw_khz,sf,cr,preamble_sym); }
inline bool LORA_TX(const uint8_t* b,size_t n){ return radio().tx(b,n); }
inline bool LORA_TX_SIMULATION(const uint8_t* b,size_t n){ return radio().tx_simulation(b,n); }
inline SimulationTransmitTrace LORA_TX_SIMULATION_TRACE(const uint8_t* b,size_t n){ return radio().tx_simulation_trace(b,n); }
inline int  LORA_RSSI(){ return radio().rssi(); }
inline bool CW_ON(double s){ return radio().tx_cw(s); }
inline bool CW_OFF(){ return true; }
inline bool IR_TX(const uint8_t* b, size_t n, int bitrate){ return ir().tx(b,n,bitrate); }
inline bool BS_MODULATE(const uint8_t* bits, size_t nbits, int bitrate){ return bs().mod(bits,nbits,bitrate); }
inline bool IR_TX_SIMULATION(const uint8_t*, size_t, int){ return true; }
inline bool BS_MODULATE_SIMULATION(const uint8_t*, size_t, int){ return true; }
inline bool RIS_SET_PHASES(const vector<uint8_t>& p){ return ris().set(p); }
inline double DutyLeftHint(){ return radio().duty_left(); }
inline double LoraAirtimeSeconds(size_t bytes){ return max(0.005, bytes / (125000.0/8.0)); }

// PHY stuff shared
} // namespace HAL

namespace phy {
  enum class Mode{ RF, BACKSCATTER, IR };
  static inline double Jpkt(Mode m,size_t B){ switch(m){ case Mode::RF: return 0.25e-3 + B*0.6e-6; case Mode::BACKSCATTER: return 0.01e-3 + B*0.02e-6; case Mode::IR: return 0.12e-3 + B*0.3e-6; } return 1.0; }
  static inline double snr_db(double gain, Mode m, double illum){ double base = 10*log10(max(1e-12, gain)); if(m==Mode::BACKSCATTER) base += (illum>0? 8.0: -6.0); if(m==Mode::IR) base -= 3.0; return base + 15.0; }
  static const double EU868_CH[] = {868.1e6,868.3e6,868.5e6,867.1e6,867.3e6,867.5e6};
}

// FHSS salato — PATCH ⬇︎
namespace HAL {
  inline double FHSS_next(uint8_t salt){
    static int i=0;
    const int N = (int)(sizeof(phy::EU868_CH)/sizeof(double));
    i = (i + 1 + (salt % 3)) % N;
    return phy::EU868_CH[i];
  }
}

// Energy
namespace energy { struct Store{ double cap_J, E; Store(double c,double e):cap_J(c),E(e){} bool can_spend(double J) const { return isfinite(J) && J>=0.0 && E>=J; } bool spend(double J){ if(can_spend(J)){ E-=J; return true;} return false;} void harvest(double W,double dt){ E=min(cap_J, E+W*dt*0.4);} double soc() const { return E/cap_J; } }; }

// Geometry/World/RIS
namespace geom { struct Vec2{ double x=0,y=0; }; static inline double dist(const Vec2&a,const Vec2&b){ double dx=a.x-b.x, dy=a.y-b.y; return sqrt(dx*dx+dy*dy); } }
namespace world {
  using geom::Vec2; struct RISTile{ Vec2 p; uint8_t phase2b=0; };
  struct World{
    double freq=868e6; vector<RISTile> ris; vector<pair<Vec2,double>> obs; double illum=0.0;
    double fs(const Vec2&a,const Vec2&b) const { double d=max(1e-1, geom::dist(a,b)); double g=1.0/(d*d); for(auto&o:obs) if(geom::dist(a,o.first)<o.second || geom::dist(b,o.first)<o.second) g*=0.1; return g; }
    double bounce(const Vec2&a,const Vec2&b,const RISTile&t) const {
      double dt=max(1e-1, geom::dist(a,t.p)), dr=max(1e-1, geom::dist(t.p,b));
      double ph=(t.phase2b%4)*(M_PI/2.0); double beam=0.6+0.4*cos(ph);
      auto norm=[&](Vec2 v){ double n=sqrt(v.x*v.x+v.y*v.y)+1e-9; v.x/=n; v.y/=n; return v; };
      Vec2 vi=norm({t.p.x-a.x, t.p.y-a.y}), vo=norm({b.x-t.p.x, b.y-t.p.y});
      double cosi=fabs(vi.x*vo.x + vi.y*vo.y);
      return 0.15*(1.0/(dt*dt*dr*dr))*beam*cosi;
    }
    double multibounce_best(const Vec2&a,const Vec2&b,int K) const {
      double best=fs(a,b); for(auto&t:ris) best=max(best, bounce(a,b,t)); if(K==1) return best;
      for(size_t i=0;i<ris.size(); ++i) for(size_t j=0;j<ris.size(); ++j){ if(i==j) continue; best=max(best, bounce(a,ris[i].p,ris[i]) * bounce(ris[i].p,b,ris[j]) * 5e1); }
      return best;
    }
  };
}
