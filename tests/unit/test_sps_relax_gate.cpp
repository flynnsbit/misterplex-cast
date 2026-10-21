#include "libmisterplex/sps_relax_gate.hpp"

#include <cstdio>
#include <cstdint>

static int g_fail = 0;

static void expect(const char* name, bool ok) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", name);
        g_fail++;
    }
}

int main() {
    using namespace misterplex;

    expect("v10 id is the relax row", spsRelaxNvencRow(kSpsRelaxV10BuildId));
    expect("v11 0x48C51FF1 is also", spsRelaxNvencRow(0x48C51FF1u));
    expect("v12 0x48CFEE31 is also", spsRelaxNvencRow(0x48CFEE31u));
    expect("480p-prod refit 0x48110015 is also", spsRelaxNvencRow(0x48110015u));
    expect("240p-prod refit 0x24A10026 is also", spsRelaxNvencRow(0x24A10026u));
    expect("selected 240p 0x24F1A2C4 is not", spsRelaxNvencRow(0x24F1A2C4u) == false);
    expect("v9 0x487A1BD1 is not", spsRelaxNvencRow(0x487A1BD1u) == false);
    expect("old relax 0x48FBCE88 is not this row", spsRelaxNvencRow(0x48FBCE88u) == false);
    expect("banned 0x48FF65F2 is not", spsRelaxNvencRow(0x48FF65F2u) == false);
    expect("banned 0x48FE178F is not", spsRelaxNvencRow(0x48FE178Fu) == false);
    expect("unknown id is not", spsRelaxNvencRow(0x48005D52u) == false);

    const FpgaBuildInfo* v10 = recognisedFpgaBuild(kSpsRelaxV10BuildId);
    const FpgaBuildInfo* v9 = recognisedFpgaBuild(0x487A1BD1u);
    expect("both rows exist and differ by id and the flag",
           v10 && v9 && v10 != v9 && v10->spsRelaxNvenc && !v9->spsRelaxNvenc &&
               v10->confVersion == v9->confVersion && v10->confVersion == 9 &&
               v10->avTrimMs == v9->avTrimMs && v10->avTrimMs == -45 &&
               v10->avResync == v9->avResync && v10->hostIdleFb == v9->hostIdleFb &&
               v10->inputNav == v9->inputNav && v10->idleBeatPlxl == v9->idleBeatPlxl &&
               v10->statusBlock == v9->statusBlock && v10->wedgePage == v9->wedgePage &&
               v10->hostIdleFb && v10->inputNav && !v10->idleBeatPlxl && v10->statusBlock &&
               v10->wedgePage);

    const auto relax = spsRelaxLimits(kSpsRelaxV10BuildId, 1);
    const auto v11lim = spsRelaxLimits(0x48C51FF1u, 1);
    const auto v12lim = spsRelaxLimits(0x48CFEE31u, 1);
    const auto plain = spsRelaxLimits(0x487A1BD1u, 1);
    const auto old = spsRelaxLimits(0x48FBCE88u, 1);
    expect("v11 limits match v10", v11lim.capRefs == relax.capRefs && v11lim.refsLimit == relax.refsLimit);
    expect("v12 limits match v10", v12lim.capRefs == relax.capRefs && v12lim.refsLimit == relax.refsLimit);
    expect("relax declared refs 2, active cap stays 1",
           relax.refsLimit == 2 && relax.capRefs == 1 && relax.decBufLimit == 2 &&
               relax.hrdAllowed && relax.picStructAllowed);
    expect("v9 stays refs 1, decbuf 1, HRD and pic_struct refused",
           plain.refsLimit == 1 && plain.decBufLimit == 1 && !plain.hrdAllowed &&
               !plain.picStructAllowed);
    expect("0x48FBCE88 is not widened on this base",
           old.refsLimit == 1 && old.decBufLimit == 1 && !old.hrdAllowed);

    expect("absent CAPS maxRefs floors to 1 and still widens declared refs",
           spsRelaxLimits(kSpsRelaxV10BuildId, 0).capRefs == 1 &&
               spsRelaxLimits(kSpsRelaxV10BuildId, 0).refsLimit == 2);
    expect("a CAPS maxRefs of 2 is already the declared limit",
           spsRelaxLimits(0x487A1BD1u, 2).refsLimit == 2 &&
               spsRelaxLimits(kSpsRelaxV10BuildId, 2).refsLimit == 2);

    expect("declared 2 accepted only on the relax row",
           spsDeclaredRefsOk(relax, 2) && !spsDeclaredRefsOk(plain, 2) &&
               !spsDeclaredRefsOk(relax, 3));
    expect("active refs stay at the CAPS floor",
           spsActiveRefsOk(relax, 1) && !spsActiveRefsOk(relax, 2));
    expect("decbuf 2 accepted only on the relax row",
           spsDecBufOk(relax, 2) && !spsDecBufOk(plain, 2) && !spsDecBufOk(relax, 3));
    expect("reorder is never relaxed", spsReorderOk(0) && !spsReorderOk(1));
    expect("HRD and pic_struct follow the row",
           spsHrdOk(relax, true) && !spsHrdOk(plain, true) && spsHrdOk(plain, false) &&
               spsPicStructOk(relax, true) && !spsPicStructOk(plain, true));

    int minBits = 32;
    uint32_t nearest = 0;
    for (const FpgaBuildInfo& id : kRecognisedFpgaBuilds) {
        if (id.id == kSpsRelaxV10BuildId)
            continue;
        const int d = __builtin_popcount(kSpsRelaxV10BuildId ^ id.id);
        if (d < minBits) {
            minBits = d;
            nearest = id.id;
        }
    }
    expect("Hamming at least 7 vs recognised ids", minBits >= 7);
    expect("14 bits from 0x48FF65F2", __builtin_popcount(kSpsRelaxV10BuildId ^ 0x48FF65F2u) == 14);
    expect("14 bits from 0x487A1BD1", __builtin_popcount(kSpsRelaxV10BuildId ^ 0x487A1BD1u) == 14);
    expect("15 bits from 0x48FE178F", __builtin_popcount(kSpsRelaxV10BuildId ^ 0x48FE178Fu) == 15);

    if (g_fail) {
        std::fprintf(stderr, "FAIL sps_relax_gate failures=%d nearest=0x%08X d=%d\n", g_fail, nearest,
                     minBits);
        return 1;
    }
    std::printf("PASS test_sps_relax_gate id=0x%08X hamming=%d nearest=0x%08X\n", kSpsRelaxV10BuildId,
                minBits, nearest);
    return 0;
}
