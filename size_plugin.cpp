// ============================================================================
//  MHW RNG Official Plugin: Monster Size & Conditional Crown Controller (`rng_size.dll`)
//  Version : 2.1.0 (SDK v2 Reference Implementation)
//  Target  : 0x169BA15 (`nEmDtTune::cSizeTable::getSizeMultiplier` @ 0x14169BA00)
//
//  Architecture & SDK Best-Practice Highlights:
//    1. Per-Monster Conditional Routing via `MonsterID` (`0..101` @ `monster + 0x12280`):
//       - `CondID = 0`             : Global policy for unspecified monsters.
//       - `CondID = monsterId + 1` : Per-species/variant policy (`1..102`, e.g. `91` Rajang ->
//                                    CondID 92, `92` Furious Rajang -> CondID 93), isolating
//                                    each monster's RNG stream/rule in multi-monster quests.
//    2. IEEE-754 Open-Interval `(0.0f, 100.0f)` Safety (`[0x00000001U, 0xFFFFFF00U]`):
//       - `cSizeTable::getSizeMultiplier` evaluates:
//           `if ((float)u32 * (100.0f / 2^32) <= (float)cumWeight) return &rows[i];`
//       - Injecting `u32 == 0` yields `0.0f <= 0.0f` (TRUE on Row 0 `88%` even when Row 0 has
//         `weight == 0`, producing illegal sizes on restricted quests like "Mew are Number One!").
//       - Injecting `u32 >= 0xFFFFFF80U` rounds `(float)u32 * (100.0f / 2^32)` to `100.0f`,
//         falling past `cumWeight == 100` into Capcom's `rows[12]` (`100%`) fallback.
//       - Mapping `[0%, 100%]` strictly into `[0x00000001U, 0xFFFFFF00U]` (`+2.328e-8f .. 99.99999f`)
//         skips all `weight == 0` disabled rows and lands cleanly inside legal `[min, max]` rows,
//         while also adapting dynamically if a quest's `cSizeTable` weights sum != 100.
//    3. Lock Discipline (Zero Lock-Order Inversion):
//       - `g_stateMx` is a strict leaf lock protecting plugin-local state only; Host API functions
//         (`g_host->...`) are NEVER invoked while holding `g_stateMx`.
// ============================================================================
#include "mhw_rng_api.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Verified MHW:I 15.23 (Steam 15539686) RVAs & Struct Offsets:
constexpr std::uint32_t  kRvaSizeTableGetMultRet   = 0x169BA15U; // Return RVA inside `cSizeTable::getSizeMultiplier`
constexpr std::uintptr_t kRvaCallerFun140F5F760Ret = 0x0F5F784U; // Return RVA inside `FUN_140f5f760` (`slotPtr[6]`)
constexpr std::uintptr_t kRvaEmSizeCalcGlobalPtr   = 0x5012908U; // `DAT_145012908` (`sEnemyTune` size table holder)
constexpr std::uintptr_t kOffMonsterId             = 0x12280U;   // `MonsterID` (`0..101`) inside `uEnemy` / `Monster`
constexpr std::uintptr_t kOffMonsterSizeTableIdx   = 0x773CU;    // `cSizeTable` index inside `Monster`
constexpr std::uint32_t  kMaxMonsterId             = 101U;       // `MonsterID::Fatalis == 101`
constexpr int            kSizeTableRowCount        = 38;         // 38 rows (`88% .. 125%`) per `cSizeTable`

// Open-interval `(0.0f, 100.0f)` IEEE-754 float32 bounds for `(float)u32 * (100.0f / 4294967296.0f)`:
//   - `kMinSafeFloatU32` (`1U`)          -> `+2.3283064e-8f` (`> 0.0f`, skips `weight == 0` rows under `<=`)
//   - `kMaxSafeFloatU32` (`0xFFFFFF00U`) -> `99.9999924f`    (`< 100.0f`, never overflows `cumWeight == 100`)
constexpr std::uint32_t  kMinSafeFloatU32          = 0x00000001U;
constexpr std::uint32_t  kMaxSafeFloatU32          = 0xFFFFFF00U;

struct MonsterEntry {
    std::uint32_t monsterId; // Runtime `*(uint32_t*)(monster + 0x12280)` (`MonsterID` enum `0..101`)
    const char*   keyword;   // Canonical CLI name (e.g. "rajang", "furious_rajang")
    const char*   shortAlias;// Optional convenient alias (e.g. "furious", "nergi", "jho")
    const char*   emCode;    // Capcom internal asset ID (e.g. "em023_00", "em023_05")
    const char*   cnName;    // Chinese localized name
    const char*   enName;    // English localized name
    const char*   variantHint; // Optional UX hint when monster has a common subspecies/variant
};

// Complete verified MHW:I 15.23 Large Monster Registry (71 species/subspecies/variants):
constexpr MonsterEntry kLargeMonsters[] = {
    {  0U, "anjanath",        "anja",      "em100_00", u8"蛮颚龙",     "Anjanath",               "fulgur_anjanath" },
    {  1U, "rathalos",        "los",       "em002_00", u8"火龙",       "Rathalos",               "azure_rathalos / silver_rathalos" },
    {  4U, "zorah",           "magdaros",  "em106_00", u8"熔山龙",     "Zorah Magdaros",         nullptr },
    {  7U, "great_jagras",    "jagras",    "em101_00", u8"大凶豺龙",   "Great Jagras",           nullptr },
    {  9U, "rathian",         "ian",       "em001_00", u8"雌火龙",     "Rathian",                "pink_rathian / gold_rathian" },
    { 10U, "pink_rathian",    "pink",      "em001_01", u8"樱火龙",     "Pink Rathian",           nullptr },
    { 11U, "azure_rathalos",  "azure",     "em002_01", u8"苍火龙",     "Azure Rathalos",         nullptr },
    { 12U, "diablos",         "blos",      "em007_00", u8"角龙",       "Diablos",                "black_diablos" },
    { 13U, "black_diablos",   "bdiablos",  "em007_01", u8"黑角龙",     "Black Diablos",          nullptr },
    { 14U, "kirin",           nullptr,     "em011_00", u8"麒麟",       "Kirin",                  nullptr },
    { 15U, "behemoth",        "behe",      "em121_00", u8"贝希摩斯",   "Behemoth",               nullptr },
    { 16U, "kushala",         "daora",     "em024_00", u8"钢龙",       "Kushala Daora",          nullptr },
    { 17U, "lunastra",        "luna",      "em026_00", u8"炎妃龙",     "Lunastra",               nullptr },
    { 18U, "teostra",         "teo",       "em027_00", u8"炎王龙",     "Teostra",                nullptr },
    { 19U, "lavasioth",       "lava",      "em036_00", u8"熔岩龙",     "Lavasioth",              nullptr },
    { 20U, "deviljho",        "jho",       "em043_00", u8"恐暴龙",     "Deviljho",               "savage_jho" },
    { 21U, "barroth",         nullptr,     "em044_00", u8"土砂龙",     "Barroth",                nullptr },
    { 22U, "uragaan",         "ura",       "em045_00", u8"爆锤龙",     "Uragaan",                nullptr },
    { 23U, "leshen",          nullptr,     "em127_00", u8"鹿首精",     "Leshen",                 "ancient_leshen" },
    { 24U, "pukei",           "pukei_pukei","em102_00",u8"毒妖鸟",     "Pukei-Pukei",            "coral_pukei" },
    { 25U, "nergigante",      "nergi",     "em103_00", u8"灭尽龙",     "Nergigante",             "ruiner_nergi" },
    { 26U, "xeno",            "xeno_jiiva","em105_00", u8"冥灯龙",     "Xeno'jiiva",             nullptr },
    { 27U, "kulu",            "kulu_ya_ku","em107_00", u8"搔鸟",       "Kulu-Ya-Ku",             nullptr },
    { 28U, "tzitzi",          "tzitzi_ya_ku","em120_00",u8"眩鸟",      "Tzitzi-Ya-Ku",           nullptr },
    { 29U, "jyuratodus",      "jyura",     "em108_00", u8"泥鱼龙",     "Jyuratodus",             nullptr },
    { 30U, "tobi",            "tobi_kadachi","em109_00",u8"飞雷龙",    "Tobi-Kadachi",           "viper_tobi" },
    { 31U, "paolumu",         "lumu",      "em110_00", u8"浮空龙",     "Paolumu",                "nightshade" },
    { 32U, "legiana",         "legi",      "em111_00", u8"风漂龙",     "Legiana",                "shrieking" },
    { 33U, "great_girros",    "girros",    "em112_00", u8"大凶颚龙",   "Great Girros",           nullptr },
    { 34U, "odogaron",        "odo",       "em113_00", u8"惨爪龙",     "Odogaron",               "ebony_odo" },
    { 35U, "radobaan",        "baan",      "em114_00", u8"骨锤龙",     "Radobaan",               nullptr },
    { 36U, "vaal",            "vaal_hazak","em115_00", u8"尸套龙",     "Vaal Hazak",             "blackveil" },
    { 37U, "dodogama",        "dodo",      "em116_00", u8"岩贼龙",     "Dodogama",               nullptr },
    { 38U, "kulve",           "kulve_taroth","em117_00",u8"绚辉龙",    "Kulve Taroth",           nullptr },
    { 39U, "bazelgeuse",      "bazel",     "em118_00", u8"爆鳞龙",     "Bazelgeuse",             "seething_bazel" },
    { 51U, "ancient_leshen",  "aleshen",   "em127_01", u8"古代鹿首精", "Ancient Leshen",         nullptr },
    { 61U, "tigrex",          nullptr,     "em032_00", u8"轰龙",       "Tigrex",                 "brute_tigrex" },
    { 62U, "nargacuga",       "narga",     "em037_00", u8"迅龙",       "Nargacuga",              nullptr },
    { 63U, "barioth",         nullptr,     "em042_00", u8"冰牙龙",     "Barioth",                "frostfang" },
    { 64U, "savage_jho",      "savage",    "em043_05", u8"惶怒恐暴龙", "Savage Deviljho",        nullptr },
    { 65U, "brachydios",      "brachy",    "em063_00", u8"碎龙",       "Brachydios",             "raging_brachy" },
    { 66U, "glavenus",        "glav",      "em080_00", u8"斩龙",       "Glavenus",               "acidic_glavenus" },
    { 67U, "acidic_glavenus", "acidic",    "em080_01", u8"硫斩龙",     "Acidic Glavenus",        nullptr },
    { 68U, "fulgur_anjanath", "fulgur",    "em100_01", u8"雷颚龙",     "Fulgur Anjanath",        nullptr },
    { 69U, "coral_pukei",     "coral",     "em102_01", u8"水妖鸟",     "Coral Pukei-Pukei",      nullptr },
    { 70U, "ruiner_nergi",    "ruiner",    "em103_05", u8"歼世灭尽龙", "Ruiner Nergigante",      nullptr },
    { 71U, "viper_tobi",      "viper",     "em109_01", u8"痹毒龙",     "Viper Tobi-Kadachi",     nullptr },
    { 72U, "nightshade",      "nightshade_paolumu", "em110_01", u8"浮眠龙", "Nightshade Paolumu", nullptr },
    { 73U, "shrieking",       "shrieking_legiana",  "em111_05", u8"霜翼风漂龙", "Shrieking Legiana", nullptr },
    { 74U, "ebony_odo",       "ebony",     "em113_01", u8"凶爪龙",     "Ebony Odogaron",         nullptr },
    { 75U, "blackveil",       "blackveil_vaal",     "em115_05", u8"雾瘴尸套龙", "Blackveil Vaal Hazak", nullptr },
    { 76U, "seething_bazel",  "seething",  "em118_05", u8"红莲爆鳞龙", "Seething Bazelgeuse",    nullptr },
    { 77U, "beotodus",        "beo",       "em122_00", u8"冰鱼龙",     "Beotodus",               nullptr },
    { 78U, "banbaro",         nullptr,     "em123_00", u8"猛牛龙",     "Banbaro",                nullptr },
    { 79U, "velkhana",        "velk",      "em124_00", u8"冰咒龙",     "Velkhana",               nullptr },
    { 80U, "namielle",        "nami",      "em125_00", u8"溟波龙",     "Namielle",               nullptr },
    { 81U, "shara",           "shara_ishvalda", "em126_00", u8"天地煌啼龙", "Shara Ishvalda",    nullptr },
    { 87U, "alatreon",        "ala",       "em050_00", u8"煌黑龙",     "Alatreon",               nullptr },
    { 88U, "gold_rathian",    "gold",      "em001_02", u8"金火龙",     "Gold Rathian",           nullptr },
    { 89U, "silver_rathalos", "silver",    "em002_02", u8"银火龙",     "Silver Rathalos",        nullptr },
    { 90U, "garuga",          "yian_garuga","em018_00",u8"黑狼鸟",     "Yian Garuga",            "scarred_garuga" },
    { 91U, "rajang",          "jang",      "em023_00", u8"金狮子",     "Rajang",                 "furious_rajang" },
    { 92U, "furious_rajang",  "furious",   "em023_05", u8"激昂金狮子", "Furious Rajang",         nullptr },
    { 93U, "brute_tigrex",    "brute",     "em032_01", u8"黑轰龙",     "Brute Tigrex",           nullptr },
    { 94U, "zinogre",         "zin",       "em057_00", u8"雷狼龙",     "Zinogre",                "stygian_zinogre" },
    { 95U, "stygian_zinogre", "stygian",   "em057_01", u8"狱狼龙",     "Stygian Zinogre",        nullptr },
    { 96U, "raging_brachy",   "raging",    "em063_05", u8"猛爆碎龙",   "Raging Brachydios",      nullptr },
    { 97U, "safi",            "safi_jiiva","em104_00", u8"冥赤龙",     "Safi'jiiva",             nullptr },
    { 99U, "scarred_garuga",  "scarred",   "em018_05", u8"战痕黑狼鸟", "Scarred Yian Garuga",    nullptr },
    {100U, "frostfang",       "frostfang_barioth", "em042_05", u8"霜刃冰牙龙", "Frostfang Barioth", nullptr },
    {101U, "fatalis",         "fata",      "em013_00", u8"黑龙",       "Fatalis",                nullptr },
};

enum class RuleMode : std::uint8_t {
    kNone     = 0, // Unmodified: uses kernel `Slot[(0 << 32) | 0x169BA15]`
    kCondSlot = 1, // Fixed normalized roll `[0..100]` stored in `Slot[(condId << 32) | 0x169BA15]`
    kExactPct = 2  // Live 38-row `cSizeTable` inverse lookup for closest legal `targetSizePct`
};

struct SizePolicy {
    RuleMode      mode          = RuleMode::kNone;
    std::uint32_t condId        = 0U;   // `0` = global, `monsterId + 1` = per-monster ConditionEnum
    std::uint32_t monsterId     = 0U;   // Valid when `condId > 0` (`condId - 1`)
    std::uint32_t fixedU32      = kMinSafeFloatU32; // Cached safe `u32` when `mode == kCondSlot`
    double        rollPct0To100 = 0.0;  // Logical `[0.0 .. 100.0]` roll when `mode == kCondSlot`
    std::int32_t  targetSizePct = 100;  // Target size% (`88 .. 125`) when `mode == kExactPct`
    bool          exactFromExactCmd = false;
    bool          isQuickMin    = false;
    bool          isQuickMax    = false;
    bool          isQuickMid    = false;
};

struct SpawnHistoryEntry {
    bool          validMonster = false;
    bool          validTable   = false;
    bool          hasDirectU32 = false;
    std::uint32_t monsterId    = 0U;
    std::uint32_t tableIdx     = 0U;
    std::uint32_t usedCondId   = 0U;
    std::uint32_t directU32    = 0U;
    std::int32_t  minPct       = 100;
    std::int32_t  maxPct       = 100;
    std::int32_t  sizePcts[kSizeTableRowCount]{};
    std::int32_t  weights[kSizeTableRowCount]{};
};

const MhwRngHostApi*    g_host   = nullptr;
MhwRngPluginId          g_selfId = 0U;

// Strict leaf mutex: NEVER invoke `g_host->...` while holding `g_stateMx`!
std::mutex              g_stateMx;
SizePolicy              g_globalPolicy{};
std::vector<SizePolicy> g_monsterPolicies;
SpawnHistoryEntry       g_recentSpawns[4]{};
std::size_t             g_recentCount = 0U;

inline bool IsEn() noexcept {
    return MhwRng_HostIsEnglish(g_host);
}

std::string FormatPolicySummary(const SizePolicy& p, bool en) {
    char buf[80]{};
    switch (p.mode) {
        case RuleMode::kCondSlot:
            if (p.isQuickMin) return en ? "Min legal crown (+0.0/100)" : u8"最小合法体型(小金档 +0.0/100)";
            if (p.isQuickMax) return en ? "Max legal crown (100.0/100)" : u8"最大合法体型(大金档 100.0/100)";
            if (p.isQuickMid) return en ? "Normal median size (50.0/100)" : u8"标准中等体型(50.0/100)";
            std::snprintf(buf, sizeof(buf), en ? "Fixed weight roll=%.1f/100" : u8"固定权重盘面=%.1f/100", p.rollPct0To100);
            return buf;
        case RuleMode::kExactPct:
            std::snprintf(buf, sizeof(buf), en ? "Exact table size=%d%%" : u8"实时查表反算体型=%d%%", p.targetSizePct);
            return buf;
        default:
            return en ? "Unlocked (vanilla)" : u8"未锁定(正常随机)";
    }
}

// ============================================================================
//  String, Wildcard & Monster Lookup Helpers
// ============================================================================

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

bool WildcardMatchI(const char* pat, const char* str) noexcept {
    if (!pat || !str) return false;
    const char* starPat = nullptr;
    const char* starStr = nullptr;
    while (*str) {
        char pc = *pat;
        char sc = *str;
        if (pc >= 'A' && pc <= 'Z') pc = static_cast<char>(pc + ('a' - 'A'));
        if (sc >= 'A' && sc <= 'Z') sc = static_cast<char>(sc + ('a' - 'A'));
        if (pc == '*') { starPat = pat++; starStr = str; }
        else if (pc == '?' || pc == sc) { ++pat; ++str; }
        else if (starPat) { pat = starPat + 1; str = ++starStr; }
        else return false;
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

bool ContainsSubstrI(const char* hay, const std::string& needleLow) noexcept {
    if (!hay || needleLow.empty()) return false;
    const std::size_t n = needleLow.size();
    for (std::size_t i = 0; hay[i] != '\0'; ++i) {
        std::size_t j = 0;
        while (j < n && hay[i + j] != '\0') {
            char c = hay[i + j];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            if (c != needleLow[j]) break;
            ++j;
        }
        if (j == n) return true;
    }
    return false;
}

std::size_t EditDistance(const std::string& a, const char* bRaw) {
    if (!bRaw) return 999U;
    const std::string b = LowerAscii(bRaw);
    const std::size_t m = a.size(), n = b.size();
    if (m == 0U) return n;
    if (n == 0U) return m;
    std::vector<std::size_t> dp(n + 1U);
    for (std::size_t j = 0; j <= n; ++j) dp[j] = j;
    for (std::size_t i = 1; i <= m; ++i) {
        std::size_t prev = dp[0];
        dp[0] = i;
        for (std::size_t j = 1; j <= n; ++j) {
            const std::size_t tmp = dp[j];
            const std::size_t cost = (a[i - 1U] == b[j - 1U]) ? 0U : 1U;
            dp[j] = std::min({dp[j] + 1U, dp[j - 1U] + 1U, prev + cost});
            prev  = tmp;
        }
    }
    return dp[n];
}

const MonsterEntry* FindMonsterById(std::uint32_t monsterId) noexcept {
    for (const auto& m : kLargeMonsters) {
        if (m.monsterId == monsterId) return &m;
    }
    return nullptr;
}

std::string FormatMonsterTag(std::uint32_t monsterId, bool validMonster, bool en = false) {
    if (!validMonster) return en ? "Unknown" : u8"未知怪物";
    if (const MonsterEntry* m = FindMonsterById(monsterId)) {
        char buf[96]{};
        if (en) {
            std::snprintf(buf, sizeof(buf), "%s (%s #%u)", m->enName, m->keyword, monsterId);
        } else {
            std::snprintf(buf, sizeof(buf), "%s(%s #%u)", m->cnName, m->keyword, monsterId);
        }
        return buf;
    }
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), en ? "Monster #%u" : u8"怪物(#%u)", monsterId);
    return buf;
}

bool ParseMonsterSelector(const std::string& rawTok,
                          std::vector<std::uint32_t>& outIds,
                          std::string& outLabel,
                          const char** outVariantHint = nullptr) {
    const bool en = IsEn();
    outIds.clear();
    outLabel.clear();
    if (outVariantHint) *outVariantHint = nullptr;
    if (rawTok.empty()) return false;

    const std::string low = LowerAscii(rawTok);

    auto pushUnique = [&](std::uint32_t id) {
        if (std::find(outIds.begin(), outIds.end(), id) == outIds.end()) {
            outIds.push_back(id);
        }
    };

    const char* idDigits = nullptr;
    if (low[0] == '#' && low.size() > 1U) idDigits = low.c_str() + 1;
    else if (low.rfind("id:", 0) == 0 && low.size() > 3U) idDigits = low.c_str() + 3;
    if (idDigits) {
        char* ep = nullptr;
        const unsigned long v = std::strtoul(idDigits, &ep, 10);
        if (ep != idDigits && *ep == '\0' && v <= kMaxMonsterId) {
            const auto mid = static_cast<std::uint32_t>(v);
            outIds.push_back(mid);
            outLabel = FormatMonsterTag(mid, true, en);
            return true;
        }
        return false;
    }

    if (low.find('*') != std::string::npos || low.find('?') != std::string::npos) {
        const std::string familyPat = (low.front() != '*' && low.back() == '*') ? ("*" + low) : low;
        for (const auto& m : kLargeMonsters) {
            if (WildcardMatchI(low.c_str(), m.keyword) ||
                (m.shortAlias && WildcardMatchI(low.c_str(), m.shortAlias)) ||
                WildcardMatchI(low.c_str(), m.emCode) ||
                WildcardMatchI(familyPat.c_str(), m.keyword)) {
                pushUnique(m.monsterId);
            }
        }
        if (!outIds.empty()) {
            if (outIds.size() == 1U) {
                outLabel = FormatMonsterTag(outIds[0], true, en);
            } else {
                char buf[96]{};
                std::snprintf(buf, sizeof(buf),
                              en ? "`%s` (%zu monsters)" : u8"`%s` 匹配 %zu 种怪物",
                              rawTok.c_str(), outIds.size());
                outLabel = buf;
            }
            return true;
        }
        return false;
    }

    for (const auto& m : kLargeMonsters) {
        if (low == m.keyword || (m.shortAlias && low == m.shortAlias) || low == m.emCode) {
            outIds.push_back(m.monsterId);
            outLabel = FormatMonsterTag(m.monsterId, true, en);
            if (outVariantHint) *outVariantHint = m.variantHint;
            return true;
        }
    }

    if (low.rfind("em", 0) == 0 && low.size() >= 3U && low.find('_') == std::string::npos) {
        char* ep = nullptr;
        const unsigned long emNum = std::strtoul(low.c_str() + 2, &ep, 10);
        if (ep != (low.c_str() + 2) && *ep == '\0' && emNum >= 1UL && emNum <= 199UL) {
            char prefix[16]{};
            std::snprintf(prefix, sizeof(prefix), "em%03lu_", emNum);
            for (const auto& m : kLargeMonsters) {
                if ( std::strncmp(m.emCode, prefix, 6U) == 0 ) {
                    pushUnique(m.monsterId);
                }
            }
            if (!outIds.empty()) {
                if (outIds.size() == 1U) {
                    outLabel = FormatMonsterTag(outIds[0], true, en);
                } else {
                    char buf[96]{};
                    std::snprintf(buf, sizeof(buf),
                                  en ? "em%03lu family (%zu)" : u8"em%03lu全系(共%zu种)",
                                  emNum, outIds.size());
                    outLabel = buf;
                }
                return true;
            }
        }
    }

    return false;
}

std::string SuggestClosestMonster(const std::string& badTok) {
    const bool en = IsEn();
    const std::string low = LowerAscii(badTok);
    if (low.size() < 3U) return {};
    const MonsterEntry* best = nullptr;
    std::size_t bestDist = 999U;
    for (const auto& m : kLargeMonsters) {
        if (ContainsSubstrI(m.keyword, low) || (m.shortAlias && ContainsSubstrI(m.shortAlias, low))) {
            return en ? std::string(m.keyword) : (std::string(m.keyword) + "(" + m.cnName + ")");
        }
        const std::size_t d1 = EditDistance(low, m.keyword);
        const std::size_t d2 = m.shortAlias ? EditDistance(low, m.shortAlias) : 999U;
        const std::size_t d  = std::min(d1, d2);
        if (d < bestDist) {
            bestDist = d;
            best     = &m;
        }
    }
    if (best && bestDist <= 3U) {
        std::string hint = en ? std::string(best->keyword) : (std::string(best->keyword) + "(" + best->cnName + ")");
        if (best->variantHint) {
            hint += en ? " or " : u8" 或 ";
            hint += best->variantHint;
        }
        return hint;
    }
    return {};
}

// ============================================================================
//  IEEE-754 Open-Interval `(0.0f, 100.0f)` Conversion & `cSizeTable` Math
// ============================================================================

// Maps `[0.0, 100.0]` strictly into `[kMinSafeFloatU32, kMaxSafeFloatU32]` (`[1U, 0xFFFFFF00U]`):
//   - `0.0`   -> `1U`          (`roll = +2.3283064e-8f > 0.0f`, NEVER hits `0.0f <= 0.0f` on `weight == 0`!)
//   - `100.0` -> `0xFFFFFF00U` (`roll =  99.9999924f < 100.0f`, NEVER overflows `cumWeight == 100`!)
std::uint32_t Roll0To100ToSafeU32(double roll0To100) noexcept {
    if (!(roll0To100 > 0.0)) return kMinSafeFloatU32;
    if (roll0To100 >= 100.0) return kMaxSafeFloatU32;
    const double scaled = (roll0To100 / 100.0) * static_cast<double>(kMaxSafeFloatU32);
    const auto u = static_cast<std::uint64_t>(scaled + 0.5);
    if (u <= kMinSafeFloatU32) return kMinSafeFloatU32;
    if (u >= kMaxSafeFloatU32) return kMaxSafeFloatU32;
    return static_cast<std::uint32_t>(u);
}

// Adapts a logical `[0.0, 100.0]` roll to tables whose positive weights sum to `totalWeight != 100`
// so `max` (`100%`) never exceeds `(float)totalWeight` and falls back to `rows[12]` (`100%`)!
std::uint32_t AdaptRollToTableWeightSum(double rollPct0To100, std::uint32_t totalWeight) noexcept {
    if (totalWeight == 0U || totalWeight == 100U) {
        return Roll0To100ToSafeU32(rollPct0To100);
    }
    const double maxSafeRoll = std::min(100.0, static_cast<double>(totalWeight) * 0.99999);
    const double targetRoll  = (rollPct0To100 / 100.0) * maxSafeRoll;
    return Roll0To100ToSafeU32(targetRoll);
}

struct LiveSpawnContext {
    bool          validMonster = false;
    bool          validTable   = false;
    std::uint32_t monsterId    = 0U;
    std::uint32_t tableIdx     = 0U;
    std::uint32_t totalWeight  = 0U;
    std::int32_t  sizePcts[kSizeTableRowCount]{};
    std::int32_t  weights[kSizeTableRowCount]{};
    std::int32_t  minAllowed   = 100;
    std::int32_t  maxAllowed   = 100;
};

bool ReadLiveSpawnContextSEH(const MhwRngDrawContext* ctx, LiveSpawnContext& out) noexcept {
    if (!ctx || !ctx->returnAddrSlot || !ctx->exeBase) return false;
    __try {
        const auto* slotPtr = static_cast<const std::uintptr_t*>(ctx->returnAddrSlot);
        // Verify exact caller stack frame of `cSizeTable::getSizeMultiplier` (`0x14169BA00`):
        //   `slotPtr[5]` = saved `RBX` (`Monster*` passed from `FUN_141ccfb70`)
        //   `slotPtr[6]` = return RVA `0x0F5F784` inside `FUN_140f5f760`
        if (slotPtr[6] != (ctx->exeBase + kRvaCallerFun140F5F760Ret)) {
            return false;
        }

        const std::uintptr_t monster = slotPtr[5];
        if (monster >= 0x100000ULL && monster <= 0x00007FFFFFFF0000ULL && (monster & 0x7ULL) == 0ULL) {
            const std::uint32_t runtimeId = *reinterpret_cast<const std::uint32_t*>(monster + kOffMonsterId);
            const std::uint32_t tblIdx    = *reinterpret_cast<const std::uint32_t*>(monster + kOffMonsterSizeTableIdx);
            out.tableIdx = tblIdx;
            if (runtimeId <= kMaxMonsterId) {
                out.monsterId    = runtimeId;
                out.validMonster = true;
            }
        }

        // Also support reading `cSizeTable` even if `monster` wasn't a valid heap pointer (e.g. synthetic C++ call):
        // Note: in `cSizeTable::getSizeMultiplier(cSizeTable* rcx)`, `cSizeTable` was passed in RCX (`slotPtr` frame),
        // and when `out.validMonster` is true we look up `holder->tables[out.tableIdx]`.
        const std::uintptr_t holder = *reinterpret_cast<const std::uintptr_t*>(ctx->exeBase + kRvaEmSizeCalcGlobalPtr);
        if (holder > 0x100000ULL && holder < 0x00007FFFFFFF0000ULL) {
            const std::uint32_t tblCount = *reinterpret_cast<const std::uint32_t*>(holder + 0xE8U);
            const std::uintptr_t tblArr  = *reinterpret_cast<const std::uintptr_t*>(holder + 0xF8U);
            if (out.tableIdx < tblCount && tblArr > 0x100000ULL && tblArr < 0x00007FFFFFFF0000ULL) {
                const std::uintptr_t cSizeTbl = *reinterpret_cast<const std::uintptr_t*>(
                    tblArr + static_cast<std::uintptr_t>(out.tableIdx) * sizeof(std::uintptr_t));
                if (cSizeTbl > 0x100000ULL && cSizeTbl < 0x00007FFFFFFF0000ULL) {
                    std::int32_t  minP = 99999, maxP = 0;
                    std::uint32_t wSum = 0U;
                    for (int i = 0; i < kSizeTableRowCount; ++i) {
                        const std::int32_t sp = *reinterpret_cast<const std::int32_t*>(cSizeTbl + 0x20U + i * 0x10U);
                        const std::int32_t wt = *reinterpret_cast<const std::int32_t*>(cSizeTbl + 0x24U + i * 0x10U);
                        out.sizePcts[i] = sp;
                        out.weights[i]  = wt;
                        if (wt > 0 && sp >= 10 && sp <= 500) {
                            wSum += static_cast<std::uint32_t>(wt);
                            if (sp < minP) minP = sp;
                            if (sp > maxP) maxP = sp;
                        }
                    }
                    if (wSum > 0U && minP <= maxP) {
                        out.totalWeight = wSum;
                        out.minAllowed  = minP;
                        out.maxAllowed  = maxP;
                        out.validTable  = true;
                    }
                }
            }
        }
        return out.validMonster || out.validTable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Exact bit-accurate mirror of `cSizeTable::getSizeMultiplier` (`0x14169BA00`):
std::int32_t SimulateSizeTableRoll(const std::int32_t sizePcts[kSizeTableRowCount],
                                   const std::int32_t weights[kSizeTableRowCount],
                                   std::uint32_t u32Val) noexcept {
    const float roll = static_cast<float>(static_cast<std::uint64_t>(u32Val)) * (100.0f / 4294967296.0f);
    std::uint32_t cum = 0U;
    for (int i = 0; i < kSizeTableRowCount; ++i) {
        if (weights[i] > 0) cum += static_cast<std::uint32_t>(weights[i]);
        if (roll <= static_cast<float>(cum)) {
            return sizePcts[i];
        }
    }
    return sizePcts[12]; // `+0xE0` = 100% fallback
}

// Inverse solver: finds the LEGAL (`weight > 0`) row closest to `targetPct` and returns
// the safe midpoint `u32` strictly inside `(cumBefore, cumBefore + weight]`.
std::uint32_t SolveU32ForTargetSizePct(const LiveSpawnContext& sc, std::int32_t targetPct) noexcept {
    if (!sc.validTable) {
        if (targetPct <= 90)  return kMinSafeFloatU32;
        if (targetPct >= 120) return kMaxSafeFloatU32;
        return 0x80000000U;
    }

    int bestIdx  = -1;
    int bestDiff = 999999;
    for (int i = 0; i < kSizeTableRowCount; ++i) {
        if (sc.weights[i] <= 0) continue; // Never select disabled 0-weight rows!
        const int diff = std::abs(sc.sizePcts[i] - targetPct);
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx  = i;
        }
    }
    if (bestIdx < 0) return 0x80000000U;

    std::uint32_t cumBefore = 0U;
    for (int i = 0; i < bestIdx; ++i) {
        if (sc.weights[i] > 0) cumBefore += static_cast<std::uint32_t>(sc.weights[i]);
    }
    const double midRoll = static_cast<double>(cumBefore) + 0.5 * static_cast<double>(sc.weights[bestIdx]);
    return Roll0To100ToSafeU32(midRoll);
}

// ============================================================================
//  Pull-Mode Synchronous Slot Callback on `0x169BA15`
//  (Zero `g_host` calls under `g_stateMx` — 100% lock-inversion free!)
// ============================================================================

void __fastcall OnSizeMultiplierDraw(const MhwRngDrawContext* ctx,
                                     MhwRngHookDecision* ioDecision,
                                     void*) {
    if (!ioDecision) return;

    LiveSpawnContext sc{};
    ReadLiveSpawnContextSEH(ctx, sc);

    std::lock_guard<std::mutex> lk(g_stateMx);

    // 1. Match Per-Monster Policy (`CondID = monsterId + 1`), falling back to Global Policy (`CondID = 0`)
    const SizePolicy* activePol = &g_globalPolicy;
    if (sc.validMonster) {
        for (const SizePolicy& mp : g_monsterPolicies) {
            if (mp.monsterId == sc.monsterId && mp.mode != RuleMode::kNone) {
                activePol = &mp;
                break;
            }
        }
    }

    bool          hasDirectU32 = false;
    std::uint32_t directU32    = 0U;

    if (activePol->mode == RuleMode::kExactPct) {
        const std::uint32_t solved = SolveU32ForTargetSizePct(sc, activePol->targetSizePct);
        ioDecision->action = kMhwRngActionReturnValue;
        ioDecision->condId = activePol->condId;
        ioDecision->value0 = solved;
        hasDirectU32       = true;
        directU32          = solved;
    } else if (activePol->mode == RuleMode::kCondSlot) {
        if (sc.validTable && sc.totalWeight > 0U && sc.totalWeight != 100U) {
            // Non-100 weight table: dynamically scale `[0..100]` into `(0, totalWeight)` so `max` never falls through!
            const std::uint32_t adapted = AdaptRollToTableWeightSum(activePol->rollPct0To100, sc.totalWeight);
            ioDecision->action = kMhwRngActionReturnValue;
            ioDecision->condId = activePol->condId;
            ioDecision->value0 = adapted;
            hasDirectU32       = true;
            directU32          = adapted;
        } else {
            // Standard `totalWeight == 100`: route directly to Kernel Composite Slot `(condId, 0x169BA15)`!
            ioDecision->action = kMhwRngActionUseCondition;
            ioDecision->condId = activePol->condId;
            hasDirectU32       = true;
            directU32          = activePol->fixedU32;
        }
    } else {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
    }

    // 2. Record spawn telemetry ring buffer (up to 4 most recent monster spawns)
    if (sc.validMonster || sc.validTable) {
        for (std::size_t i = 3U; i > 0U; --i) {
            g_recentSpawns[i] = g_recentSpawns[i - 1U];
        }
        if (g_recentCount < 4U) ++g_recentCount;

        SpawnHistoryEntry& h = g_recentSpawns[0];
        h.validMonster = sc.validMonster;
        h.validTable   = sc.validTable;
        h.hasDirectU32 = hasDirectU32;
        h.monsterId    = sc.monsterId;
        h.tableIdx     = sc.tableIdx;
        h.usedCondId   = ioDecision->condId;
        h.directU32    = directU32;
        h.minPct       = sc.minAllowed;
        h.maxPct       = sc.maxAllowed;
        std::memcpy(h.sizePcts, sc.sizePcts, sizeof(h.sizePcts));
        std::memcpy(h.weights,  sc.weights,  sizeof(h.weights));
    }
}

bool __fastcall FormatSizeSlotStatus(const MhwRngSlotSnapshot* snap,
                                     char* outBuf,
                                     std::size_t bufCap,
                                     void*) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    const bool en = IsEn();
    std::lock_guard<std::mutex> lk(g_stateMx);

    if (snap->condId == 0U) {
        if (g_globalPolicy.mode != RuleMode::kNone) {
            std::snprintf(outBuf, bufCap, "%s", FormatPolicySummary(g_globalPolicy, en).c_str());
            return true;
        }
        if (!g_monsterPolicies.empty()) {
            std::snprintf(outBuf, bufCap,
                          en ? "Per-monster rules (%zu active)" : u8"按怪物条件分流(%zu种专属规则)",
                          g_monsterPolicies.size());
            return true;
        }
        return false;
    }

    for (const auto& mp : g_monsterPolicies) {
        if (mp.condId == snap->condId && mp.mode != RuleMode::kNone) {
            const MonsterEntry* m = FindMonsterById(mp.monsterId);
            if (m) {
                if (en) {
                    std::snprintf(outBuf, bufCap, "%s (%s): %s", m->enName, m->keyword, FormatPolicySummary(mp, true).c_str());
                } else {
                    std::snprintf(outBuf, bufCap, u8"%s(%s): %s", m->cnName, m->keyword, FormatPolicySummary(mp, false).c_str());
                }
            } else {
                std::snprintf(outBuf, bufCap, "Monster#%u: %s", mp.monsterId, FormatPolicySummary(mp, en).c_str());
            }
            return true;
        }
    }
    return false;
}

enum class ParseErrKind : std::uint8_t {
    kOk = 0,
    kBadSyntax,
    kReversedOrderHint,
    kOutOfRange
};

ParseErrKind ParsePolicyTokens(int argc,
                               const char* const* argv,
                               std::uint32_t condId,
                               std::uint32_t monsterId,
                               SizePolicy& outPol) {
    if (argc < 1) return ParseErrKind::kBadSyntax;
    const std::string t0 = LowerAscii(argv[0]);

    const bool isKnownSingleKeyword =
        (t0 == "min" || t0 == "small" || t0 == "mini" ||
         t0 == "max" || t0 == "giant" || t0 == "large" || t0 == "big" ||
         t0 == "mid" || t0 == "normal" || t0 == "std");

    if (argc >= 2 && t0 != "exact") {
        std::vector<std::uint32_t> tmpIds;
        std::string tmpLbl;
        if (ParseMonsterSelector(argv[argc - 1], tmpIds, tmpLbl)) {
            return ParseErrKind::kReversedOrderHint;
        }
        return ParseErrKind::kBadSyntax;
    }

    if (isKnownSingleKeyword) {
        if (t0 == "min" || t0 == "small" || t0 == "mini") {
            outPol = SizePolicy{RuleMode::kCondSlot, condId, monsterId, kMinSafeFloatU32, 0.0, 88, false, true, false, false};
            return ParseErrKind::kOk;
        }
        if (t0 == "max" || t0 == "giant" || t0 == "large" || t0 == "big") {
            outPol = SizePolicy{RuleMode::kCondSlot, condId, monsterId, kMaxSafeFloatU32, 100.0, 125, false, false, true, false};
            return ParseErrKind::kOk;
        }
        outPol = SizePolicy{RuleMode::kCondSlot, condId, monsterId, 0x80000000U, 50.0, 100, false, false, false, true};
        return ParseErrKind::kOk;
    }

    std::string exactStr;
    if (t0 == "exact") {
        if (argc != 2) return ParseErrKind::kBadSyntax;
        exactStr = argv[1];
    } else if (t0.rfind("exact:", 0) == 0 && t0.size() > 6U) {
        if (argc != 1) return ParseErrKind::kBadSyntax;
        exactStr = std::string(argv[0]).substr(6U);
    }
    if (!exactStr.empty()) {
        if (!exactStr.empty() && exactStr.back() == '%') exactStr.pop_back();
        char* ep = nullptr;
        const long pct = std::strtol(exactStr.c_str(), &ep, 10);
        if (ep == exactStr.c_str() || *ep != '\0') return ParseErrKind::kBadSyntax;
        if (pct < 50L || pct > 250L) return ParseErrKind::kOutOfRange;
        outPol = SizePolicy{RuleMode::kExactPct, condId, monsterId, 0x80000000U, 50.0, static_cast<std::int32_t>(pct), true, false, false, false};
        return ParseErrKind::kOk;
    }

    char* ep = nullptr;
    const double roll = std::strtod(argv[0], &ep);
    if (ep != argv[0] && std::isfinite(roll) && (*ep == '\0' || (*ep == '%' && ep[1] == '\0'))) {
        if (roll < 0.0 || roll > 100.0) return ParseErrKind::kOutOfRange;
        const std::uint32_t safeU32 = Roll0To100ToSafeU32(roll);
        outPol = SizePolicy{RuleMode::kCondSlot, condId, monsterId, safeU32, roll, 100, false, false, false, false};
        return ParseErrKind::kOk;
    }

    return ParseErrKind::kBadSyntax;
}

void SyncPolicyToKernelSlot(const SizePolicy& pol) {
    if (!g_host) return;
    if (pol.mode == RuleMode::kCondSlot) {
        g_host->SetCondFixedValue(g_selfId, kRvaSizeTableGetMultRet, pol.condId, pol.fixedU32);
    } else {
        g_host->SetCondMasterDerived(g_selfId, kRvaSizeTableGetMultRet, pol.condId);
    }
}

void PrintSpawnHistory(bool showAll) {
    const bool en = IsEn();
    SpawnHistoryEntry entries[4]{};
    std::size_t count = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        count = g_recentCount;
        for (std::size_t i = 0; i < count; ++i) entries[i] = g_recentSpawns[i];
    }

    if (count == 0U) {
        if (en) {
            g_host->ChatPrint(showAll
                                  ? "[size] No monster spawns recorded yet; enter a quest to record cSizeTable rolls"
                                  : "[size] Usage: `$rng size min|max|0~100|exact 123` or `$rng size furious_rajang min`");
        } else {
            g_host->ChatPrint(showAll
                                  ? u8"[size] 尚无刷怪记录；进任务刷出大型怪物后会自动记录其任务体型表与抽中倍率"
                                  : u8"[size] 用法: `$rng size min|max|0~100|exact 123` 或按怪 `$rng size furious_rajang min`");
        }
        return;
    }

    const std::size_t limit = showAll ? count : 1U;
    for (std::size_t i = 0; i < limit; ++i) {
        const SpawnHistoryEntry& e = entries[i];
        const std::string mTag = FormatMonsterTag(e.monsterId, e.validMonster, en);

        std::int32_t rolledPct = -1;
        if (e.validTable) {
            if (e.hasDirectU32) {
                rolledPct = SimulateSizeTableRoll(e.sizePcts, e.weights, e.directU32);
            } else if (i == 0U) {
                MhwRngSlotSnapshot snap{};
                if (g_host->GetCondSlotInfo(kRvaSizeTableGetMultRet, e.usedCondId, &snap) && snap.totalCalls > 0ULL) {
                    rolledPct = SimulateSizeTableRoll(e.sizePcts, e.weights, snap.lastValue);
                }
            }
        }

        const char* prefix = showAll ? (en ? "[size] Log" : u8"[size] 记录") : (en ? "[size] Last:" : u8"[size] 最近刷怪:");
        if (e.validTable && rolledPct > 0) {
            if (showAll) {
                g_host->ChatPrintf(en ? "%s#%zu: %s | Table:%d%%~%d%% | Rolled:%d%% [Cond:%u]"
                                      : u8"%s#%zu: %s | 任务表允许:%d%%~%d%% | 抽出:%d%% [Cond:%u]",
                                   prefix, i + 1U, mTag.c_str(), e.minPct, e.maxPct, rolledPct, e.usedCondId);
            } else {
                g_host->ChatPrintf(en ? "%s %s | Table:%d%%~%d%% | Rolled:%d%%"
                                      : u8"%s %s | 任务表允许:%d%%~%d%% | 抽出:%d%%",
                                   prefix, mTag.c_str(), e.minPct, e.maxPct, rolledPct);
            }
        } else if (e.validTable) {
            g_host->ChatPrintf(en ? "%s %s | Table:%d%%~%d%%"
                                  : u8"%s %s | 任务表允许:%d%%~%d%%",
                               prefix, mTag.c_str(), e.minPct, e.maxPct);
        } else {
            g_host->ChatPrintf("%s %s [Cond:%u]", prefix, mTag.c_str(), e.usedCondId);
        }
    }
}

void PrintSizeStatus() {
    const bool en = IsEn();
    SizePolicy              globalSnap{};
    std::vector<SizePolicy> monsterSnaps;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        globalSnap   = g_globalPolicy;
        monsterSnaps = g_monsterPolicies;
    }

    g_host->ChatPrintf(en ? "[size] Global: %s | Per-monster rules: %zu"
                          : u8"[size] 全局策略: %s | 按怪物专属规则: %zu 条",
                       FormatPolicySummary(globalSnap, en).c_str(),
                       monsterSnaps.size());

    const std::size_t showN = std::min<std::size_t>(monsterSnaps.size(), 4U);
    for (std::size_t i = 0; i < showN; ++i) {
        const auto& mp = monsterSnaps[i];
        g_host->ChatPrintf(en ? "[size] Rule [Cond:%u] %s -> %s" : u8"[size] 专属[Cond:%u] %s -> %s",
                           mp.condId,
                           FormatMonsterTag(mp.monsterId, true, en).c_str(),
                           FormatPolicySummary(mp, en).c_str());
    }
    if (monsterSnaps.size() > showN) {
        g_host->ChatPrintf(en ? "[size] ...and %zu more rule(s) active" : u8"[size] ...另有 %zu 条怪物专属规则生效中",
                           monsterSnaps.size() - showN);
    }

    PrintSpawnHistory(false);
}

void Cmd_Monsters(int argc, const char* const* argv) {
    const bool en = IsEn();
    if (argc < 1 || argv[0][0] == '\0') {
        if (en) {
            g_host->ChatPrint("[size] Popular: rajang | furious_rajang (furious) | zinogre | stygian | nergi | ruiner");
            g_host->ChatPrint("[size] Popular: brachy | raging | velkhana | alatreon | fatalis | safi | kulve | teostra");
            g_host->ChatPrint("[size] Search:  `$rng size monsters <keyword>` (wildcards like `rajang*` supported)");
        } else {
            g_host->ChatPrint(u8"[size] 热门词: rajang(金狮子) furious_rajang/furious(激昂) zinogre stygian(狱狼) nergi ruiner(歼世)");
            g_host->ChatPrint(u8"[size] 热门词: brachy raging(猛爆) velkhana alatreon fatalis safi kulve teostra lunastra kushala");
            g_host->ChatPrint(u8"[size] 搜索任意怪物名/亚种: `$rng size monsters <英文词>` (支持通配符如 `rajang*` 匹配全系)");
        }
        return;
    }
    const std::string kw = LowerAscii(argv[0]);
    std::size_t shown = 0U;
    for (const auto& m : kLargeMonsters) {
        if (ContainsSubstrI(m.keyword, kw) ||
            (m.shortAlias && ContainsSubstrI(m.shortAlias, kw)) ||
            ContainsSubstrI(m.emCode, kw) ||
            ContainsSubstrI(m.enName, kw) ||
            ContainsSubstrI(m.cnName, kw)) {
            if (en) {
                if (m.shortAlias) {
                    g_host->ChatPrintf("[size] #%u %s (alias:%s | %s) : %s",
                                       m.monsterId, m.keyword, m.shortAlias, m.emCode, m.enName);
                } else {
                    g_host->ChatPrintf("[size] #%u %s (%s) : %s", m.monsterId, m.keyword, m.emCode, m.enName);
                }
            } else {
                if (m.shortAlias) {
                    g_host->ChatPrintf(u8"[size] #%u %s (简写:%s | %s) : %s",
                                       m.monsterId, m.keyword, m.shortAlias, m.emCode, m.cnName);
                } else {
                    g_host->ChatPrintf(u8"[size] #%u %s (%s) : %s",
                                       m.monsterId, m.keyword, m.emCode, m.cnName);
                }
            }
            if (++shown >= 6U) break;
        }
    }
    if (shown == 0U) {
        g_host->ChatPrintf(en ? "[size] No large monster matched `%s`" : u8"[size] 未找到包含 `%s` 的大型怪物", argv[0]);
    }
}

void __fastcall Cmd_Size(int argc, const char* const* argv, const char*, void*) {
    if (!g_host) return;
    const bool en = IsEn();
    if (argc == 0) {
        PrintSizeStatus();
        return;
    }

    const std::string a0 = LowerAscii(argv[0]);
    if (a0 == "status" || a0 == "info") { PrintSizeStatus(); return; }
    if (a0 == "history" || a0 == "recent") { PrintSpawnHistory(true); return; }
    if (a0 == "monsters" || a0 == "monster" || a0 == "list_monsters") { Cmd_Monsters(argc - 1, argv + 1); return; }
    if (a0 == "help" || a0 == "?") {
        if (en) {
            g_host->ChatPrint("[size] Global:  `$rng size min` (mini crown) | `max` (giant) | `0~100` | `exact 123`");
            g_host->ChatPrint("[size] Monster: `$rng size rajang max` | `furious_rajang min` | family: `rajang*`");
            g_host->ChatPrint("[size] Info:    `$rng size monsters [kw]` | `history` | `clear` | `<monster> clear`");
        } else {
            g_host->ChatPrint(u8"[size] 全局: `$rng size min`(最小合法小金) | `max`(大金) | `0~100` | `exact 123`(反算)");
            g_host->ChatPrint(u8"[size] 按怪: `$rng size rajang max` | `$rng size furious_rajang min` | 全系:`rajang*`");
            g_host->ChatPrint(u8"[size] 查询/清理: `$rng size monsters [词]` | `history` | `clear` | `<怪物> clear`");
        }
        return;
    }
    if (a0 == "off" || a0 == "clear" || a0 == "reset") {
        if (argc >= 2) {
            std::vector<std::uint32_t> targetIds;
            std::string label;
            if (ParseMonsterSelector(argv[1], targetIds, label)) {
                {
                    std::lock_guard<std::mutex> lk(g_stateMx);
                    g_monsterPolicies.erase(
                        std::remove_if(g_monsterPolicies.begin(), g_monsterPolicies.end(),
                                       [&](const SizePolicy& p) {
                                           return std::find(targetIds.begin(), targetIds.end(), p.monsterId) != targetIds.end();
                                       }),
                        g_monsterPolicies.end());
                }
                for (std::uint32_t mid : targetIds) {
                    g_host->SetCondMasterDerived(g_selfId, kRvaSizeTableGetMultRet, mid + 1U);
                }
                g_host->ChatPrintf(en ? "[size] Cleared rule for %s" : u8"[size] 已清除 %s 的专属体型规则", label.c_str());
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_globalPolicy = SizePolicy{};
            g_monsterPolicies.clear();
        }
        g_host->ClearSlotAllConditions(kRvaSizeTableGetMultRet);
        g_host->ChatPrint(en ? "[size] Cleared all size rules -> vanilla RNG"
                             : u8"[size] 已清空全部全局与按怪物专属体型规则 -> 恢复正常随机");
        return;
    }

    std::vector<std::uint32_t> targetMonsterIds;
    std::string                selectorLabel;
    const char*                variantHint = nullptr;
    if (ParseMonsterSelector(argv[0], targetMonsterIds, selectorLabel, &variantHint)) {
        if (argc < 2) {
            g_host->ChatPrintf(en ? "[size] Usage: `$rng size %s <min|max|mid|0~100|exact 123|clear>`"
                                  : u8"[size] 用法: `$rng size %s <min|max|mid|0~100|exact 123|clear>`",
                               argv[0]);
            return;
        }
        const std::string subAct = LowerAscii(argv[1]);
        if (subAct == "clear" || subAct == "off" || subAct == "reset") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_monsterPolicies.erase(
                    std::remove_if(g_monsterPolicies.begin(), g_monsterPolicies.end(),
                                   [&](const SizePolicy& p) {
                                       return std::find(targetMonsterIds.begin(), targetMonsterIds.end(), p.monsterId) != targetMonsterIds.end();
                                   }),
                    g_monsterPolicies.end());
            }
            for (std::uint32_t mid : targetMonsterIds) {
                g_host->SetCondMasterDerived(g_selfId, kRvaSizeTableGetMultRet, mid + 1U);
            }
            g_host->ChatPrintf(en ? "[size] Removed rule for %s" : u8"[size] 已移除 %s 的专属体型条件规则", selectorLabel.c_str());
            return;
        }

        SizePolicy proto{};
        const ParseErrKind pe = ParsePolicyTokens(argc - 1, argv + 1, 0U, 0U, proto);
        if (pe == ParseErrKind::kOutOfRange) {
            g_host->ChatPrint(en ? "[size] Out of range: roll `0~100`, `exact` size% `50~250`"
                                 : u8"[size] 数值超出范围: 权重比例请输 `0~100`，`exact` 体型请输 `50~250`");
            return;
        }
        if (pe != ParseErrKind::kOk) {
            g_host->ChatPrint(en ? "[size] Invalid args; use min / max / mid / 0~100 / exact <pct> / clear"
                                 : u8"[size] 参数无效；支持 `min` / `max` / `mid` / `0~100` / `exact <体型%>` / `clear`");
            return;
        }

        std::vector<SizePolicy> applied;
        applied.reserve(targetMonsterIds.size());
        for (std::uint32_t mid : targetMonsterIds) {
            SizePolicy mp = proto;
            mp.monsterId  = mid;
            mp.condId     = mid + 1U;
            applied.push_back(mp);
            SyncPolicyToKernelSlot(mp);
        }

        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            for (const SizePolicy& mp : applied) {
                bool found = false;
                for (auto& existing : g_monsterPolicies) {
                    if (existing.monsterId == mp.monsterId) {
                        existing = mp;
                        found    = true;
                        break;
                    }
                }
                if (!found) g_monsterPolicies.push_back(mp);
            }
        }

        const std::string sum = FormatPolicySummary(proto, en);
        if (applied.size() == 1U && variantHint != nullptr) {
            g_host->ChatPrintf(en ? "[size] Set [Cond:%u] %s -> %s (for variant: `%s` or `%s*`)"
                                  : u8"[size] 已设专属 [Cond:%u] %s -> %s (亚种/特殊个体请用 `%s` 或 `%s*`)",
                               applied[0].condId, selectorLabel.c_str(), sum.c_str(), variantHint, argv[0]);
        } else if (applied.size() == 1U) {
            g_host->ChatPrintf(en ? "[size] Set [Cond:%u] %s -> %s (other monsters unaffected)"
                                  : u8"[size] 已设专属 [Cond:%u] %s -> %s (同场其它怪物不受影响)",
                               applied[0].condId, selectorLabel.c_str(), sum.c_str());
        } else {
            g_host->ChatPrintf(en ? "[size] Set %s -> %s (other monsters unaffected)"
                                  : u8"[size] 已批量设 %s -> %s (同场其它怪物不受影响)",
                               selectorLabel.c_str(), sum.c_str());
        }
        return;
    }

    SizePolicy globalPol{};
    const ParseErrKind pe = ParsePolicyTokens(argc, argv, 0U, 0U, globalPol);
    if (pe == ParseErrKind::kReversedOrderHint) {
        g_host->ChatPrintf(en ? "[size] Order hint: put monster first, e.g. `$rng size %s %s`"
                              : u8"[size] 顺序提示: 按怪锁体型请把怪物名放前面，例如 `$rng size %s %s`",
                           argv[argc - 1], argv[0]);
        return;
    }
    if (pe == ParseErrKind::kOutOfRange) {
        g_host->ChatPrint(en ? "[size] Out of range: roll `0~100`, `exact` size% `50~250`"
                             : u8"[size] 数值超出范围: 权重比例请输 `0~100`，`exact` 体型请输 `50~250`");
        return;
    }
    if (pe != ParseErrKind::kOk) {
        const std::string suggestion = SuggestClosestMonster(argv[0]);
        if (!suggestion.empty()) {
            g_host->ChatPrintf(en ? "[size] Unknown monster `%s`; did you mean %s? (`$rng size monsters <kw>`)"
                                  : u8"[size] 未知怪物 `%s`；你是否指 %s？(查表: `$rng size monsters <词>`)",
                               argv[0], suggestion.c_str());
        } else {
            g_host->ChatPrintf(en ? "[size] Unknown arg `%s`; type `$rng size help` or `$rng size monsters`"
                                  : u8"[size] 未知参数 `%s`；输 `$rng size help` 或 `$rng size monsters` 查看用法",
                               argv[0]);
        }
        return;
    }

    SyncPolicyToKernelSlot(globalPol);
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_globalPolicy = globalPol;
    }
    g_host->ChatPrintf(en ? "[size] Global size rule -> %s" : u8"[size] 已设全局体型规则 -> %s",
                       FormatPolicySummary(globalPol, en).c_str());
}

void __fastcall Complete_Size(const char*,
                              int argc,
                              const char* const* argv,
                              const char* prefix,
                              MhwRngCompletionSinkFn addCand,
                              void* sinkCtx,
                              void*) {
    if (!addCand) return;
    const std::string prefLow = prefix ? LowerAscii(prefix) : std::string();
    const std::size_t prefLen = prefLow.size();
    const bool        wantEm  = (prefLow.rfind("em", 0) == 0);
    auto emitMonsters = [&]() {
        for (const auto& m : kLargeMonsters) {
            addCand(m.keyword, sinkCtx);
            if (prefLen >= 2U && m.shortAlias) {
                const std::size_t kwLen = std::strlen(m.keyword);
                const std::size_t alLen = std::strlen(m.shortAlias);
                const bool samePref = (kwLen >= alLen && std::strncmp(m.keyword, m.shortAlias, alLen) == 0) ||
                                      (alLen >= kwLen && std::strncmp(m.shortAlias, m.keyword, kwLen) == 0);
                if (!samePref) {
                    addCand(m.shortAlias, sinkCtx);
                }
            }
            if (wantEm && m.emCode) {
                addCand(m.emCode, sinkCtx);
            }
        }
    };

    if (argc == 0) {
        static const char* const kTop[] = {
            "status", "history", "monsters", "help", "clear", "min", "mid", "max", "exact"
        };
        for (const char* t : kTop) addCand(t, sinkCtx);
        emitMonsters();
        return;
    }
    if (argc == 1 && argv && argv[0]) {
        const std::string a0 = LowerAscii(argv[0]);
        if (a0 == "clear" || a0 == "off" || a0 == "reset" ||
            a0 == "monsters" || a0 == "monster" || a0 == "list_monsters") {
            emitMonsters();
            return;
        }
        const bool isStandaloneGlobal =
            (a0 == "status" || a0 == "info" || a0 == "history" || a0 == "recent" ||
             a0 == "help" || a0 == "?" ||
             a0 == "min" || a0 == "small" || a0 == "mini" ||
             a0 == "max" || a0 == "giant" || a0 == "large" || a0 == "big" ||
             a0 == "mid" || a0 == "normal" || a0 == "std" || a0 == "exact");
        if (!isStandaloneGlobal) {
            std::vector<std::uint32_t> tmpIds;
            std::string tmpLbl;
            if (ParseMonsterSelector(argv[0], tmpIds, tmpLbl)) {
                static const char* const kActions[] = { "min", "mid", "max", "exact", "clear" };
                for (const char* a : kActions) addCand(a, sinkCtx);
            }
        }
    }
}

const MhwRngPluginDescriptor kPluginDesc = {
    kMhwRngApiVersion,
    "size",
    u8"怪物体型与条件大小金插件",
    "2.3.0",
    u8"支持全71种大型怪物按MonsterID条件锁定大小金、0权重非法行保护、cSizeTable实时反算与多语言"
};

} // namespace

MHW_RNG_PLUGIN_EXPORT const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo() {
    return &kPluginDesc;
}

MHW_RNG_PLUGIN_EXPORT bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId) {
    if (!host || host->apiVersion != kMhwRngApiVersion) return false;
    g_host   = host;
    g_selfId = selfId;

    MhwRng_RegisterPluginInfoBi(
        host, selfId,
        u8"怪物体型与条件大小金插件",
        "Monster Size & Crown Controller",
        u8"全71种大型怪物按MonsterID条件锁定大小金与cSizeTable反算",
        "Per-monster conditional crown/size control & live cSizeTable solver (71 monsters)");

    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaSizeTableGetMultRet,
        "em.size",
        u8"怪物体型大小金倍率抽取 (由 size 插件托管)",
        "Monster crown/size multiplier roll (managed by size plugin)",
        &FormatSizeSlotStatus,
        nullptr);
    host->RegisterSlotHook(selfId, kRvaSizeTableGetMultRet, &OnSizeMultiplierDraw, nullptr);
    MhwRng_RegisterSubcommandBi(
        host, selfId,
        "size",
        "$rng size [monster|*] <min|max|0~100|exact 123|clear>",
        u8"怪物体型操纵(支持全71怪按ID条件分流/防0权重非法体型/实时反算)",
        "Control monster size/crowns per species with legal table inverse solver",
        &Cmd_Size,
        nullptr);
    MhwRng_RegisterSubcommandCompleter(host, selfId, "size", &Complete_Size, nullptr);
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    std::lock_guard<std::mutex> lk(g_stateMx);
    g_globalPolicy = SizePolicy{};
    g_monsterPolicies.clear();
    g_recentCount = 0U;
    g_host        = nullptr;
    g_selfId      = 0U;
}
