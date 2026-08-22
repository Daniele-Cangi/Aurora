// aurora_batch_test.cpp
// Batch test runner for Aurora: adaptive, stress, RIS scaling, and channel degradation tests
#define AURORA_NO_MAIN
#include "aurora_x.cpp"
#include <fstream>
#include <iomanip>

// Helper to run a single simulation and extract metrics
template<typename... Args>
struct TestResult {
    double deadline, reliability, duty;
    int ris_tiles;
    bool delivered;
    double energy_src, energy_dst, soc_src, soc_dst, delivery_time, fec_overhead, snr_avg;
};

TestResult<> run_sim(double deadline, double reliability, double duty, int ris_tiles, double channel_degrade=0.0) {
    // Set up intention string
    std::ostringstream oss;
    oss << "deadline:" << deadline << ";reliability:" << reliability << ";duty:" << duty << ";ris:" << ris_tiles << ";";
    std::string intention = oss.str();
    Engine eng;
    eng.I = Intention::parse(intention);
    // Simula peggioramento canale se richiesto
    if(channel_degrade > 0) eng.net.W.illum = channel_degrade;
    auto t0 = std::chrono::steady_clock::now();
    eng.init(intention);
    bool delivered = eng.run();
    auto t1 = std::chrono::steady_clock::now();
    double delivery_time = std::chrono::duration<double>(t1-t0).count();
    // Raccogli metriche (personalizza in base a Engine/Node)
    double energy_src = eng.net.get("SRC")->bat.E;
    double energy_dst = eng.net.get("DST")->bat.E;
    double soc_src = eng.net.get("SRC")->bat.soc();
    double soc_dst = eng.net.get("DST")->bat.soc();
    double fec_overhead = eng.K; // semplificato: simboli FEC
    double snr_avg = 0; // da calcolare se disponibile
    return {deadline, reliability, duty, ris_tiles, delivered, energy_src, energy_dst, soc_src, soc_dst, delivery_time, fec_overhead, snr_avg};
}

int main() {
    std::ofstream csv("aurora_results.csv");
    csv << "deadline,reliability,duty,ris_tiles,delivered,energy_src,energy_dst,soc_src,soc_dst,delivery_time,fec_overhead,snr_avg\n";

    // 1. Adaptive Performance Test
    for(double deadline : {1, 5, 10, 20, 30}) {
        for(double reliability : {0.7, 0.8, 0.9, 0.95, 0.99}) {
            auto r = run_sim(deadline, reliability, 0.05, 16);
            csv << r.deadline << "," << r.reliability << "," << r.duty << "," << r.ris_tiles << "," << r.delivered << "," << r.energy_src << "," << r.energy_dst << "," << r.soc_src << "," << r.soc_dst << "," << r.delivery_time << "," << r.fec_overhead << "," << r.snr_avg << "\n";
        }
    }
    // 2. Stress Test (Duty)
    for(double duty : {0.01, 0.05, 0.1, 0.2, 0.3}) {
        auto r = run_sim(10, 0.95, duty, 16);
    csv << r.deadline << "," << r.reliability << "," << r.duty << "," << r.ris_tiles << "," << r.delivered << "," << r.energy_src << "," << r.energy_dst << "," << r.soc_src << "," << r.soc_dst << "," << r.delivery_time << "," << r.fec_overhead << "," << r.snr_avg << "\n";
    }
    // 3. RIS Scaling Test
    for(int ris : {0, 4, 8, 16, 24, 32}) {
        auto r = run_sim(10, 0.95, 0.05, ris);
    csv << r.deadline << "," << r.reliability << "," << r.duty << "," << r.ris_tiles << "," << r.delivered << "," << r.energy_src << "," << r.energy_dst << "," << r.soc_src << "," << r.soc_dst << "," << r.delivery_time << "," << r.fec_overhead << "," << r.snr_avg << "\n";
    }
    // 4. Channel Degradation Test
    for(double ch : std::initializer_list<double>{0, 0.2, 0.4, 0.6, 0.8, 1.0}) {
        auto r = run_sim(10, 0.95, 0.05, 16, ch);
    csv << r.deadline << "," << r.reliability << "," << r.duty << "," << r.ris_tiles << "," << r.delivered << "," << r.energy_src << "," << r.energy_dst << "," << r.soc_src << "," << r.soc_dst << "," << r.delivery_time << "," << r.fec_overhead << "," << r.snr_avg << "\n";
    }
    csv.close();
    return 0;
}
