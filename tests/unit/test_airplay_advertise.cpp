// Delayed network: AirPlay mDNS/RAOP must wait, then register, then re-register
// if the default-route address changes. The daemon calls this plan from
// advertiseOnce(), including from the worker, so a boot that beats DHCP does
// not stick on the first failure.
#include "gcast/airplay_fe.hpp"

#include <cstdio>
#include <fstream>
#include <string>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

int main() {
    using misterplex::gcast::AirplayAdvertisePlan;
    using misterplex::gcast::airplayAdvertiseDelayMs;
    using misterplex::gcast::planAirplayAdvertise;
    int fails = 0;
    auto check = [&](bool cond, const char* what) {
        if (!cond) {
            std::printf("FAIL %s\n", what);
            ++fails;
        }
    };

    std::string registered;
    int prev = 0;
    for (int waits = 0; waits < 5; ++waits) {
        const AirplayAdvertisePlan plan = planAirplayAdvertise(registered, "", waits);
        check(plan.action == AirplayAdvertisePlan::Action::Wait, "no address waits");
        check(plan.delayMs == airplayAdvertiseDelayMs(waits), "backoff matches the schedule");
        if (waits > 0)
            check(plan.delayMs > prev, "backoff grows while the network is down");
        prev = plan.delayMs;
    }
    check(airplayAdvertiseDelayMs(0) == 1000, "first wait is 1s");
    check(airplayAdvertiseDelayMs(1) == 2000, "second wait is 2s");
    check(airplayAdvertiseDelayMs(2) == 4000, "third wait is 4s");
    check(airplayAdvertiseDelayMs(3) == 8000, "fourth wait is 8s");
    check(airplayAdvertiseDelayMs(4) == 15000, "backoff caps at 15s");
    check(airplayAdvertiseDelayMs(9) == 15000, "backoff stays capped");

    const auto linkLocal = planAirplayAdvertise(registered, "169.254.8.9", 1);
    check(linkLocal.action == AirplayAdvertisePlan::Action::Wait, "link-local is not a local address");
    const auto unspecified = planAirplayAdvertise(registered, "0.0.0.0", 0);
    check(unspecified.action == AirplayAdvertisePlan::Action::Wait, "0.0.0.0 is not a local address");
    const auto loopback = planAirplayAdvertise(registered, "127.0.0.1", 0);
    check(loopback.action == AirplayAdvertisePlan::Action::Wait, "loopback is not a local address");

    const auto up = planAirplayAdvertise(registered, "192.0.2.12", 4);
    check(up.action == AirplayAdvertisePlan::Action::Register, "address after the wait registers");
    registered = "192.0.2.12";
    const auto hold = planAirplayAdvertise(registered, "192.0.2.12", 0);
    check(hold.action == AirplayAdvertisePlan::Action::Hold, "same address does not re-register");
    const auto moved = planAirplayAdvertise(registered, "192.0.2.5", 0);
    check(moved.action == AirplayAdvertisePlan::Action::Reregister, "a new address re-registers");
    const auto gone = planAirplayAdvertise(registered, "", 0);
    check(gone.action == AirplayAdvertisePlan::Action::Wait, "a lost address waits again");

    const std::string path = std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/gcast/airplay_fe.cpp";
    std::ifstream in(path);
    const std::string cpp((std::istreambuf_iterator<char>(in)), {});
    check(!cpp.empty(), "read airplay_fe.cpp");
    check(cpp.find("planAirplayAdvertise") != std::string::npos, "daemon uses the plan");
    // The no-AirPlay stub defines the same names first. The real bodies are last.
    const auto worker = cpp.rfind("void AirplayFrontend::worker()");
    check(worker != std::string::npos, "worker exists");
    if (worker != std::string::npos)
        check(cpp.substr(worker, 500).find("advertiseOnce()") != std::string::npos,
              "worker retries advertisement");
    const auto start = cpp.rfind("bool AirplayFrontend::start(");
    check(start != std::string::npos, "start exists");
    if (start != std::string::npos) {
        const auto stop = cpp.find("void AirplayFrontend::stop(", start);
        const std::string body = cpp.substr(start, stop == std::string::npos ? 4000 : stop - start);
        check(body.find("advertiseOnce()") != std::string::npos, "start advertises");
        check(body.find("dnssd_register_raop") == std::string::npos,
              "start still registers mDNS only once");
    }
    const auto again = cpp.rfind("void AirplayFrontend::advertiseOnce()");
    check(again != std::string::npos, "advertiseOnce exists");
    if (again != std::string::npos) {
        const std::string body = cpp.substr(again, 4500);
        check(body.find("dnssd_register_raop") != std::string::npos, "retry registers RAOP");
        check(body.find("dnssd_register_airplay") != std::string::npos, "retry registers AirPlay");
        check(body.find("dnssd_unregister_raop") != std::string::npos, "address change unregisters");
        check(body.find("mDNS/RAOP waiting for a local address") != std::string::npos,
              "delayed network is logged");
    }

    if (fails) {
        std::printf("test_airplay_advertise: FAIL %d\n", fails);
        return 1;
    }
    std::printf("test_airplay_advertise: OK\n");
    return 0;
}
