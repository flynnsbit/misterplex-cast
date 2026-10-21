#include "libmisterplex/sps_reject.hpp"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

int main() {
    using misterplex::isPlex480pPresentBank;
    using misterplex::spsReject480i;
    using misterplex::spsRejectCode480pFromField;

    CHECK(std::string(spsRejectCode480pFromField("profile_idc")) == "profile");
    CHECK(std::string(spsRejectCode480pFromField("level_idc")) == "level");
    CHECK(std::string(spsRejectCode480pFromField("vui_parameters")) == "vui");
    CHECK(std::string(spsRejectCode480pFromField("max_num_ref_frames")).empty());
    CHECK(std::string(spsRejectCode480pFromField("entropy_coding_mode_flag")).empty());
    CHECK(std::string(spsRejectCode480pFromField("nal_unit_type")).empty());
    CHECK(std::string(spsRejectCode480pFromField("profile_idc")) != "profile_idc");
    CHECK(isPlex480pPresentBank(640, 480));
    CHECK(!isPlex480pPresentBank(320, 240));
    CHECK(!isPlex480pPresentBank(1280, 720));
    CHECK(!isPlex480pPresentBank(624, 480));
    CHECK(!isPlex480pPresentBank(720, 480));

    CHECK(!spsReject480i(false, false, true, 640, 480));
    CHECK(!spsReject480i(false, false, true, 320, 240));
    CHECK(!spsReject480i(true, false, true, 720, 480));
    CHECK(!spsReject480i(true, false, true, 624, 454));
    CHECK(!spsReject480i(true, false, true, 624, 480));
    CHECK(spsReject480i(true, false, true, 640, 480));
    CHECK(spsReject480i(false, true, true, 640, 480));
    CHECK(!spsReject480i(true, true, false, 640, 480));
    CHECK(!spsReject480i(true, false, true, 720, 480));

    if (fails) {
        std::fprintf(stderr, "%d FAIL\n", fails);
        return 1;
    }
    std::printf("test_sps_reject: OK\n");
    return 0;
}
