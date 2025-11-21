#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <cstdio>
#include <algorithm>
#include <map>
#include <exception> // for std::exception
#include <cmath>     // for fabsf

#include "imgui.h"
#include "arcdps_structs.h"
#include "json.hpp"
using json = nlohmann::json;

static constexpr uint32_t PLUGIN_SIG = 0xC0CD0F15;
static const char* PLUGIN_NAME = "Squad Cooldowns";
static const char* PLUGIN_VER = "1.03";

static constexpr uint32_t BUFF_ALACRITY = 30328;
static constexpr uint32_t BUFF_CHILL = 722;

#ifndef CBTS_CHANGEDEAD
#define CBTS_CHANGEDEAD 4
#endif

#ifndef CBTS_CHANGEDOWN
#define CBTS_CHANGEDOWN 5
#endif

#ifndef CBTS_EXITCOMBAT
#define CBTS_EXITCOMBAT 2
#endif

#ifndef CBTS_LOGEND
#define CBTS_LOGEND 10
#endif

#ifndef ACTV_NONE
#define ACTV_NONE 0
#endif

#ifndef ACTV_START
#define ACTV_START 1
#endif

#ifndef ACTV_CANCEL_FIRE
#define ACTV_CANCEL_FIRE 2
#endif

#ifndef ACTV_CANCEL_CANCEL
#define ACTV_CANCEL_CANCEL 3
#endif

#ifndef ACTV_RESET
#define ACTV_RESET 4
#endif

static std::unordered_map<uint32_t, float> g_hard_override_cd = {
    { 12569, 120.f },
    { 62965, 20.f },
    { 10545, 40.f },
};

static bool g_share_enabled = true;
static std::string g_room = "bags";
static std::string g_server_host = "relay.ethevia.com";
static int g_server_port = 443;
static bool g_use_https = true;

static constexpr float NET_OFFSET = 3.5f;
static constexpr float CANCEL_COOLDOWN = 1.5f;

static inline double now_s() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

// Pending CD fetch requests (non-blocking for main thread)
static std::mutex g_cd_mutex;
static std::unordered_set<uint32_t> g_cd_pending;


static void request_cd_fetch(uint32_t sid) {
    std::lock_guard<std::mutex> lk(g_cd_mutex);
    g_cd_pending.insert(sid);  // set semantics: no duplicates
}

static bool pop_next_cd_request(uint32_t& out_sid) {
    std::lock_guard<std::mutex> lk(g_cd_mutex);
    if (g_cd_pending.empty()) return false;
    auto it = g_cd_pending.begin();
    out_sid = *it;
    g_cd_pending.erase(it);
    return true;
}



struct SlotTimer {
    uint32_t skillid = 0;
    std::string name;
    float  base_cd = 0.f;
    double last_cast_s = -1.0;
    double last_update_s = -1.0;
    float  elapsed = 0.f;

    bool   cancel_active = false;
    double cancel_start_s = -1.0;

    void on_cast(double now_s_val) {
        // Real cast -> normal cooldown
        last_cast_s = now_s_val;
        last_update_s = now_s_val;
        elapsed = 0.f;
        cancel_active = false;
        cancel_start_s = -1.0;
    }

    void start_cancel_cd(double now_s_val) {
        // Cancelled cast -> short fake cooldown, independent of base_cd
        last_cast_s = -1.0;
        last_update_s = -1.0;
        elapsed = 0.f;
        cancel_active = true;
        cancel_start_s = now_s_val;
    }

    void advance(double now_s_val, bool has_alac, bool has_chill) {
        // Fake cancel cooldown is handled entirely in predict_left
        if (cancel_active)
            return;

        if (last_cast_s < 0 || base_cd <= 0) return;
        if (last_update_s < 0) last_update_s = last_cast_s;

        double dt = now_s_val - last_update_s;
        if (dt <= 0.0) return;

        float speed = 1.0f;
        if (has_alac)  speed *= 1.25f;
        if (has_chill) speed *= 0.60f;

        elapsed += float(dt * speed);
        if (elapsed > base_cd) elapsed = base_cd;

        last_update_s = now_s_val;
    }

    float predict_left(double now_s_val, bool has_alac, bool has_chill) {
        // --- Fake cancel cooldown path (no boon scaling) ---
        if (cancel_active) {
            if (cancel_start_s < 0.0) {
                // Shouldn’t happen, but treat as ready
                return 0.f;
            }

            double dt = now_s_val - cancel_start_s;
            if (dt >= CANCEL_COOLDOWN) {
                // Stay in "cancel mode" but report 0s left -> READY
                return 0.f;
            }

            float left = float(CANCEL_COOLDOWN - dt);
            if (left < 0.f) left = 0.f;
            return left;
        }

        // --- Normal cooldown path ---
        advance(now_s_val, has_alac, has_chill);

        if (last_cast_s < 0 || base_cd <= 0) return -1.f;

        float remaining = base_cd - elapsed;
        if (remaining <= 0.f) return 0.f;

        float left = remaining - NET_OFFSET;
        return left < 0.f ? 0.f : left;
    }
};



struct TrackedEntry {
    bool enabled = true;
    uint32_t skillid = 0;
    float base_cd = 0.f;
    std::string label = "Label";
};

struct SelfContext {
    uint64_t self_instid = 0;
    bool has_alacrity = false;
    bool has_chill = false;
    uint32_t subgroup = 0;
};

struct PeerEntry {
    std::string label;
    bool ready = false;
    float left = -1.f;
};

struct Peer {
    std::string id;
    std::string name;
    std::string account;
    uint32_t prof = 0;
    uint32_t subgroup = 0;
    std::vector<PeerEntry> entries;
};

static std::mutex g_mutex;
static SelfContext g_self;
static std::unordered_map<uint32_t, SlotTimer> g_by_skill;

static void advance_all_timers_locked(double now_s_val) {
    const bool has_alac = g_self.has_alacrity;
    const bool has_chill = g_self.has_chill;
    for (auto& kv : g_by_skill) {
        kv.second.advance(now_s_val, has_alac, has_chill);
    }
}

static std::vector<TrackedEntry> g_tracked;
static int g_pick_row = -1;
static double g_pick_armed_until_s = 0.0;
static uint64_t g_last_ev_ms = 0;
static uint64_t g_pick_not_before_ms = 0;

static std::string g_client_id;
static std::string g_assigned_name = "cds";
static std::vector<Peer> g_peers;

static std::unordered_map<uint32_t, std::vector<std::string>> g_group_order;
static std::unordered_set<uint32_t> g_group_order_dirty;
static bool g_overlay_enabled = true;
static bool g_in_map_change = false;
static bool g_options_drawn_this_frame = false;

static bool  g_tracked_open_prev = false;
static float g_last_content_bottom_y = 0.0f;
static bool  g_tracked_added_row = false;

static uint32_t g_self_prof = 0;
static std::string g_self_charname;
static std::string g_self_accountname;
static std::unordered_set<std::string> g_squad_accounts;
static std::unordered_set<std::string> g_dead_accounts;

static json g_cached_raw;
static double g_last_label_edit_s = -1.0;
static bool g_label_save_pending = false;
static constexpr double LABEL_SAVE_DELAY_S = 30.0;
static std::atomic<bool> g_settings_dirty{ false };


static wchar_t* (__cdecl* arc_e0)() = nullptr;
static void(__cdecl* arc_e3)(char*) = nullptr;
static void arc_log(const char* s) { if (arc_e3) arc_e3(const_cast<char*>(s)); }

#ifndef IMGUI_VERSION_NUM
#define IMGUI_VERSION_NUM 0
#endif

static ImVec4 prof_color(uint32_t prof) {
    auto c = [](unsigned rgb)->ImVec4 {
        return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
            ((rgb >> 8) & 0xFF) / 255.0f,
            (rgb & 0xFF) / 255.0f, 1.0f);
        };
    switch (prof) {
    case 1: return c(0x72C1D9);
    case 2: return c(0xFFD166);
    case 3: return c(0xD09C59);
    case 4: return c(0x8CDC82);
    case 5: return c(0xC08F95);
    case 6: return c(0xF68A87);
    case 7: return c(0xB679D5);
    case 8: return c(0x52A76F);
    case 9: return c(0xD16E5A);
    default: return ImVec4(1, 1, 1, 1);
    }
}

static const char* prof_name(uint32_t prof) {
    switch (prof) {
    case 1: return "Guard";
    case 2: return "War";
    case 3: return "Engi";
    case 4: return "Ranger";
    case 5: return "Thief";
    case 6: return "Ele";
    case 7: return "Mes";
    case 8: return "Necro";
    case 9: return "Rev";
    default: return "Unknown";
    }
}

static std::wstring dll_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW((HMODULE)&__ImageBase, buf, MAX_PATH);
    std::wstring p(buf);
    size_t p1 = p.find_last_of(L'\\');
    size_t p2 = p.find_last_of(L'/');
    size_t pos = (p1 == std::wstring::npos)
        ? p2
        : (p2 == std::wstring::npos ? p1 : (p1 > p2 ? p1 : p2));
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

static std::wstring settings_path() {
    return dll_dir() + L"\\arcdps_cooldowns.json";
}

static std::string read_file_utf8(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s;
    s.resize((size_t)n);
    fread(s.data(), 1, (size_t)n, f);
    fclose(f);
    return s;
}

static void write_file_utf8(const std::wstring& path, const std::string& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

static void save_settings_all() {
    json j;
    j["client_id"] = g_client_id;
    j["assigned_name"] = g_assigned_name;
    j["room"] = g_room;
    j["server_host"] = g_server_host;
    j["server_port"] = g_server_port;
    j["share_enabled"] = g_share_enabled;
    j["use_https"] = g_use_https;
    j["overlay_enabled"] = g_overlay_enabled;

    j["tracked"] = json::array();
    for (auto& e : g_tracked) {
        json t;
        t["enabled"] = e.enabled;
        t["skillid"] = e.skillid;
        t["base_cd"] = e.base_cd;
        t["label"] = e.label;
        j["tracked"].push_back(t);
    }

    json grp = json::object();
    for (auto& kv : g_group_order) {
        json arr = json::array();
        for (auto& id : kv.second) arr.push_back(id);
        grp[std::to_string(kv.first)] = std::move(arr);
    }
    j["group_order"] = std::move(grp);

    write_file_utf8(settings_path(), j.dump(2));
}

static void load_settings_all() {
    std::string s = read_file_utf8(settings_path());
    if (s.empty()) return;
    try {
        auto j = json::parse(s);
        if (j.contains("client_id")) g_client_id = j["client_id"].get<std::string>();
        if (j.contains("assigned_name")) g_assigned_name = j["assigned_name"].get<std::string>();
        if (j.contains("room")) g_room = j["room"].get<std::string>();
        if (j.contains("server_host")) g_server_host = j["server_host"].get<std::string>();
        if (j.contains("server_port")) g_server_port = j["server_port"].get<int>();
        if (j.contains("share_enabled")) g_share_enabled = j["share_enabled"].get<bool>();
        if (j.contains("use_https")) g_use_https = j["use_https"].get<bool>();
        if (j.contains("overlay_enabled")) g_overlay_enabled = j["overlay_enabled"].get<bool>();

        g_tracked.clear();
        if (j.contains("tracked")) {
            for (auto& t : j["tracked"]) {
                TrackedEntry e;
                e.enabled = t.value("enabled", true);
                e.skillid = t.value("skillid", 0u);
                e.base_cd = t.value("base_cd", 0.f);
                e.label = t.value("label", std::string("Label"));
                g_tracked.push_back(e);
            }
        }

        g_group_order.clear();
        if (j.contains("group_order") && j["group_order"].is_object()) {
            for (auto& kv : j["group_order"].items()) {
                uint32_t prof = (uint32_t)std::stoul(kv.key());
                std::vector<std::string> order;
                for (auto& v : kv.value()) order.push_back(v.get<std::string>());
                g_group_order[prof] = std::move(order);
            }
        }
    }
    catch (...) {}
}

static bool http_post_json(const std::string& host, int port, bool secure,
    const std::wstring& path, const std::string& body,
    std::string* out) {
    bool ok = false;
    HINTERNET hS = nullptr, hC = nullptr, hR = nullptr;
    std::string resp;

    hS = WinHttpOpen(L"ArcCooldowns/0.81", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) goto cleanup;

    hC = WinHttpConnect(hS, std::wstring(host.begin(), host.end()).c_str(),
        (INTERNET_PORT)port, 0);
    if (!hC) goto cleanup;

    hR = WinHttpOpenRequest(hC,
        L"POST",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) goto cleanup;

    {
        std::wstring hdr = L"Content-Type: application/json\r\n";
        if (!WinHttpSendRequest(hR, hdr.c_str(), (DWORD)hdr.size(),
            (LPVOID)body.data(), (DWORD)body.size(),
            (DWORD)body.size(), 0)) goto cleanup;
        if (!WinHttpReceiveResponse(hR, nullptr)) goto cleanup;

        DWORD avail = 0;
        do {
            if (!WinHttpQueryDataAvailable(hR, &avail)) break;
            if (!avail) break;
            size_t old = resp.size();
            resp.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(hR, resp.data() + old, avail, &read)) break;
            resp.resize(old + read);
        } while (avail > 0);

        ok = true;
    }

cleanup:
    if (ok && out) *out = resp;
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    return ok;
}

static bool http_get(const std::string& host, int port, bool secure,
    const std::wstring& path, std::string* out) {
    bool ok = false;
    HINTERNET hS = nullptr, hC = nullptr, hR = nullptr;
    std::string resp;

    hS = WinHttpOpen(L"ArcCooldowns/0.81", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) goto cleanup;

    hC = WinHttpConnect(hS, std::wstring(host.begin(), host.end()).c_str(),
        (INTERNET_PORT)port, 0);
    if (!hC) goto cleanup;

    hR = WinHttpOpenRequest(hC,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hR) goto cleanup;

    if (!WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hR, nullptr)) goto cleanup;

    DWORD avail = 0;
    do {
        if (!WinHttpQueryDataAvailable(hR, &avail)) break;
        if (!avail) break;
        size_t old = resp.size();
        resp.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hR, resp.data() + old, avail, &read)) break;
        resp.resize(old + read);
    } while (avail > 0);

    ok = true;

cleanup:
    if (ok && out) *out = resp;
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    return ok;
}

// -------------------- JSON helper --------------------

static bool parse_root_object(const std::string& resp_raw, json& jr) {
    try {
        if (resp_raw.empty()) return false;

        // Strip any embedded NULs just in case
        std::string resp;
        resp.reserve(resp_raw.size());
        for (unsigned char c : resp_raw) {
            if (c == '\0') continue;
            resp.push_back(static_cast<char>(c));
        }

        if (resp.empty()) return false;

        size_t start = resp.find('{');
        if (start == std::string::npos) return false;

        size_t end = resp.rfind('}');
        if (end == std::string::npos || end < start) return false;

        std::string sub = resp.substr(start, end - start + 1);
        jr = json::parse(sub);
        return true;
    }
    catch (const std::exception& e) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[sqcd] JSON parse error: %s", e.what());
        arc_log(buf);
    }
    catch (...) {
        arc_log("[sqcd] JSON parse error: unknown");
    }
    return false;
}

// -----------------------------------------------------

static std::unordered_map<uint32_t, float> g_api_cd_cache;
static std::unordered_set<uint32_t> g_api_cd_tried;

static float parse_recharge_from_skill_json(const json& j) {
    if (j.contains("facts") && j["facts"].is_array()) {
        for (auto& f : j["facts"]) {
            if (f.is_object()) {
                std::string ty = f.value("type", std::string(""));
                if (ty == "Recharge") {
                    if (f.contains("value")) {
                        try { return (float)f["value"].get<double>(); }
                        catch (...) {}
                    }
                }
            }
        }
    }
    if (j.contains("ammo")) {
        try {
            float v = j["ammo"].value("recharge_time", 0.0f);
            if (v > 0) return v;
        }
        catch (...) {}
    }
    if (j.contains("recharge")) {
        try {
            float v = (float)j["recharge"].get<double>();
            if (v > 0) return v;
        }
        catch (...) {}
    }
    return 0.f;
}

static float fetch_skill_recharge_api(uint32_t skillid) {
    std::wstring path = L"/v2/skills?id=" + std::to_wstring(skillid);
    std::string resp;
    if (!http_get("api.guildwars2.com", 443, true, path, &resp)) return 0.f;
    try {
        json j = json::parse(resp);
        if (j.is_object()) {
            return parse_recharge_from_skill_json(j);
        }
        else if (j.is_array() && !j.empty()) {
            return parse_recharge_from_skill_json(j[0]);
        }
    }
    catch (...) {}
    return 0.f;
}

static float get_base_cd_for_skill(uint32_t sid, float row_base) {
    // 1) hard override wins
    auto itH = g_hard_override_cd.find(sid);
    if (itH != g_hard_override_cd.end())
        return itH->second;

    // 2) explicit row base (manual override)
    if (row_base > 0.f)
        return row_base;

    // 3) cached from previous API fetch
    auto it = g_api_cd_cache.find(sid);
    if (it != g_api_cd_cache.end())
        return it->second;

    // 4) not known yet -> ask background thread to fetch it
    request_cd_fetch(sid);

    // non-blocking: return 0 for "unknown" so caller can show "waiting"
    return 0.f;
}


static float compute_left_for(uint32_t sid, float row_base, double now) {
    auto it = g_by_skill.find(sid);
    if (it == g_by_skill.end()) return -1.f;

    SlotTimer& st = it->second;

    // --- Fake cancel cooldown path: ignore base_cd completely ---
    if (st.cancel_active) {
        // no boon scaling on cancel-cooldown
        return st.predict_left(now, false, false);
    }

    // --- Normal cooldown path (needs a valid base_cd) ---
    const float base = get_base_cd_for_skill(sid, row_base);
    if (base <= 0.f) return -1.f;

    st.base_cd = base;

    const bool has_alac = g_self.has_alacrity;
    const bool has_chill = g_self.has_chill;

    return st.predict_left(now, has_alac, has_chill);
}


static bool is_probable_junk_name(const char* nm) {
    if (!nm || !*nm) return false;
    static const char* bads[] = {
        "Weapon Draw","Weapon Stow","Weapon Swap","Dodge","Mount","Dismount",
        "Aura","Swiftness","Superspeed","Regeneration","Resolution","Vigor",
        "Protection","Might","Fury","Quickness","Alacrity","Stability",
        "Resistance","Aegis","Barrier","Stow Weapon","Draw Weapon",
        "Leader of The Pact III","Leader of The Pact II","Leader of The Pact I",
    };
    for (auto* b : bads) {
        if (strstr(nm, b) != nullptr) return true;
    }
    return false;
}

static void __cdecl on_combat(cbtevent* ev, ag* src, ag* dst,
    const char* skillname, uint64_t id, uint64_t rev) {

    // ---- IDENTITY / MAP CHANGE HANDSHAKE ----
    if (!ev) {
        // Full map change / log reset
        if (!src && !dst) {
            std::scoped_lock lk(g_mutex);
            g_in_map_change = true;
            g_squad_accounts.clear();
            g_self_accountname.clear();
            g_dead_accounts.clear();
            g_self.has_alacrity = false;
            g_self.has_chill = false;
            return;
        }

        // Agent info for self
        if (dst && dst->self) {
            std::scoped_lock lk(g_mutex);
            g_self_prof = dst->prof;
            g_self.self_instid = dst->id;
            g_self.subgroup = dst->team;

            if (src && src->name && *src->name) {
                g_self_charname = src->name;
            }
            else if (dst->name && *dst->name) {
                g_self_charname = dst->name;
            }

            if (dst->name && *dst->name) {
                g_self_accountname = dst->name;
                g_squad_accounts.insert(g_self_accountname);
            }
        }
        return;
    }

    // ---- SQUAD MEMBERSHIP TRACKING ----
    {
        std::scoped_lock lk(g_mutex);
        g_in_map_change = false;

        auto record_member = [&](ag* a) {
            if (!a || !a->name || !*a->name) return;
            if (a->team != 0) {
                g_squad_accounts.insert(a->name);
            }
            };
        record_member(src);
        record_member(dst);
    }

    g_last_ev_ms = ev->time;
    const double now = now_s();

    // ---- CLEAR ALAC/CHILL ON DOWN/DEAD/EXIT/LOGEND ----
    if ((ev->is_statechange == CBTS_CHANGEDOWN ||
        ev->is_statechange == CBTS_CHANGEDEAD) &&
        dst && dst->self) {

        std::scoped_lock lk(g_mutex);
        g_self.has_alacrity = false;
        g_self.has_chill = false;
    }

    if ((ev->is_statechange == CBTS_EXITCOMBAT ||
        ev->is_statechange == CBTS_LOGEND) &&
        dst && dst->self) {

        std::scoped_lock lk(g_mutex);
        g_self.has_alacrity = false;
        g_self.has_chill = false;
    }

    // ---- TRACK ALAC / CHILL BUFFS ----
    if (ev->is_statechange == CBTS_NONE &&
        ev->is_buff == 1 &&
        dst && dst->self) {

        std::scoped_lock lk(g_mutex);

        if (ev->skillid == BUFF_ALACRITY) {
            g_self.has_alacrity = (ev->is_buffremove == 0);
        }
        else if (ev->skillid == BUFF_CHILL) {
            g_self.has_chill = (ev->is_buffremove == 0);
        }
    }

    const bool is_self = ((src && src->self) || (dst && dst->self));
    bool picked = false;

    if (is_self) {
        std::unique_lock<std::mutex> lk(g_mutex);

        // Keep subgroup up to date
        uint32_t team_src = (src ? src->team : 0);
        uint32_t team_dst = (dst ? dst->team : 0);
        uint32_t new_team = team_src ? team_src : team_dst;
        if (new_team != 0) {
            g_self.subgroup = new_team;
        }

        // ---- PICK MODE (Add tracked skill) ----
        if (g_pick_row >= 0 && g_pick_row < (int)g_tracked.size()) {
            if (now <= g_pick_armed_until_s) {
                if (ev->skillid != 0 &&
                    ev->is_buff == 0 &&
                    ev->time > g_pick_not_before_ms &&
                    !is_probable_junk_name(skillname)) {

                    auto& row = g_tracked[g_pick_row];

                    row.skillid = ev->skillid;
                    row.label = (skillname && *skillname)
                        ? std::string(skillname)
                        : ("skill " + std::to_string(ev->skillid));

                    auto itH = g_hard_override_cd.find(ev->skillid);
                    if (itH != g_hard_override_cd.end())
                        row.base_cd = itH->second;

                    g_pick_row = -1;
                    g_pick_armed_until_s = 0.0;
                    picked = true;
                }
            }
            else {
                g_pick_row = -1;
                g_pick_armed_until_s = 0.0;
            }
        }

        // ---- COOLDOWN LOGIC WITH ACTIVATION GUARD ----
        if (ev->skillid != 0 &&
            ev->is_buff == 0 &&
            !is_probable_junk_name(skillname)) {

            const uint32_t sid = ev->skillid;

            switch (ev->is_activation) {
            case ACTV_START:
            case ACTV_CANCEL_FIRE:
                // Real cast -> full cooldown
            {
                SlotTimer& st = g_by_skill[sid];
                st.skillid = sid;
                st.name = (skillname && *skillname)
                    ? skillname
                    : (std::string("skill ") + std::to_string(sid));
                st.on_cast(now);
            }
            break;

            case ACTV_CANCEL_CANCEL:
            case ACTV_RESET:
                // Cancelled cast -> short fake cooldown
            {
                SlotTimer& st = g_by_skill[sid];
                st.skillid = sid;
                st.name = (skillname && *skillname)
                    ? skillname
                    : (std::string("skill ") + std::to_string(sid));
                st.start_cancel_cd(now);
            }
            break;

            default:
                // ACTV_NONE or others -> ignore for CD
                break;
            }
        }

        lk.unlock();
    }

    if (picked) {
        g_settings_dirty.store(true, std::memory_order_relaxed);
    }
}

static std::thread g_net_thread;
static bool g_net_alive = false;
static bool g_initialized = false;

static std::string make_guid() {
    GUID g;
    CoCreateGuid(&g);
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static void ensure_group_membership_locked() {
    std::unordered_map<uint32_t, std::unordered_set<std::string>> have;
    for (auto& kv : g_group_order) {
        have[kv.first] = std::unordered_set<std::string>(kv.second.begin(), kv.second.end());
    }
    for (auto& p : g_peers) {
        uint32_t pr = p.prof;
        if (g_group_order.find(pr) == g_group_order.end())
            g_group_order[pr] = {};
        if (!have[pr].count(p.id)) {
            g_group_order[pr].push_back(p.id);
            have[pr].insert(p.id);
        }
    }
    std::unordered_set<std::string> all_ids_now;
    for (auto& p : g_peers) all_ids_now.insert(p.id);
    for (auto& kv : g_group_order) {
        auto& vec = kv.second;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const std::string& id) {
            return all_ids_now.find(id) == all_ids_now.end();
            }), vec.end());
    }
}

static void parse_peers_from_json_locked(const json& jr) {
    if (jr.contains("groupOrder") && jr["groupOrder"].is_object()) {
        g_group_order.clear();
        for (auto& kv : jr["groupOrder"].items()) {
            uint32_t prof = (uint32_t)std::stoul(kv.key());
            std::vector<std::string> order;
            for (auto& v : kv.value()) order.push_back(v.get<std::string>());
            g_group_order[prof] = std::move(order);
        }
    }

    std::vector<Peer> peers;

    auto parse_one = [&](const json& pj) {
        Peer p;

        // ID: clientId preferred, fall back to legacy "id"
        if (pj.contains("clientId") && pj["clientId"].is_string()) {
            p.id = pj["clientId"].get<std::string>();
        }
        else if (pj.contains("id") && pj["id"].is_string()) {
            p.id = pj["id"].get<std::string>();
        }
        else {
            p.id.clear();
        }

        p.prof = pj.value("prof", 0u);
        p.subgroup = pj.value("subgroup", 0u);

        // name may be null / missing
        if (pj.contains("name") && pj["name"].is_string()) {
            p.name = pj["name"].get<std::string>();
        }
        else {
            p.name = "unknown";
        }

        // account can be null on old clients
        if (pj.contains("account") && pj["account"].is_string()) {
            p.account = pj["account"].get<std::string>();
        }
        else {
            p.account.clear();
        }

        if (pj.contains("entries") && pj["entries"].is_array()) {
            for (auto& ej : pj["entries"]) {
                PeerEntry e;

                if (ej.contains("label") && ej["label"].is_string()) {
                    e.label = ej["label"].get<std::string>();
                }
                else {
                    e.label.clear();
                }

                e.ready = ej.value("ready", false);

                if (ej.contains("left") && ej["left"].is_number()) {
                    e.left = (float)ej["left"].get<double>();
                }
                else {
                    e.left = -1.f;
                }

                p.entries.push_back(e);
            }
        }

        if (!p.id.empty()) {
            peers.push_back(std::move(p));
        }
        };

    if (jr.contains("peers")) {
        const auto& px = jr["peers"];
        if (px.is_array()) {
            for (auto& pj : px) parse_one(pj);
        }
        else if (px.is_object()) {
            for (auto& kv : px.items()) {
                json pj = kv.value();
                pj["clientId"] = kv.key();
                parse_one(pj);
            }
        }
    }
    else if (jr.contains("clients") && jr["clients"].is_array()) {
        for (auto& pj : jr["clients"]) parse_one(pj);
    }
    else if (jr.is_array()) {
        for (auto& pj : jr) parse_one(pj);
    }
    else if (jr.contains("rooms") && jr["rooms"].is_object()) {
        auto it = jr["rooms"].find(g_room);
        if (it != jr["rooms"].end() && it->is_array()) {
            for (auto& pj : *it) parse_one(pj);
        }
    }

    g_peers.swap(peers);
    ensure_group_membership_locked();
}

static void inject_self_if_missing_locked() {
    const bool have_self =
        std::any_of(g_peers.begin(), g_peers.end(), [&](const Peer& p) { return p.id == g_client_id; });

    if (have_self) return;

    Peer self;
    self.id = g_client_id;
    self.name = !g_self_charname.empty()
        ? g_self_charname
        : (g_assigned_name.empty() ? std::string("me") : g_assigned_name);
    self.prof = g_self_prof;
    self.subgroup = g_self.subgroup;

    double now = now_s();

    for (auto& e : g_tracked) {
        if (!e.enabled || e.skillid == 0) continue;
        float left = compute_left_for(e.skillid, e.base_cd, now);
        PeerEntry pe;
        pe.label = e.label;
        pe.ready = (left >= 0.f && left <= 0.5f) || (g_by_skill.find(e.skillid) == g_by_skill.end());
        pe.left = (left < 0.f ? -1.f : left);
        self.entries.push_back(pe);
    }

    g_peers.push_back(std::move(self));
    ensure_group_membership_locked();
}

// ----------------- NET LOOP (patched) -----------------

static void net_loop() {
    if (g_client_id.empty()) {
        g_client_id = make_guid();
        save_settings_all();
    }

    auto last_push = std::chrono::steady_clock::now();
    auto last_pull = std::chrono::steady_clock::now();

    while (g_net_alive) {
        auto now_tp = std::chrono::steady_clock::now();

        // ---- PUSH /update ----
        if (g_share_enabled &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_push).count() >= 500) {

            last_push = now_tp;
            {
                uint32_t sid;
                // drain the queue (or you can do just one per loop if you prefer)
                while (pop_next_cd_request(sid)) {
                    float cd = fetch_skill_recharge_api(sid);  // blocking is OK here
                    if (cd > 0.f) {
                        std::scoped_lock lk(g_mutex);
                        g_api_cd_cache[sid] = cd;
                    }
                }
            }

            json payload;
            payload["room"] = g_room;
            payload["clientId"] = g_client_id;
            payload["pluginVer"] = PLUGIN_VER;

            {
                std::scoped_lock lk(g_mutex);
                payload["name"] = (!g_self_charname.empty() ? g_self_charname : g_assigned_name);
                payload["prof"] = g_self_prof;
                payload["subgroup"] = g_self.subgroup;
                if (!g_self_accountname.empty()) {
                    payload["account"] = g_self_accountname;
                }

                if (!g_group_order_dirty.empty()) {
                    json orders = json::object();
                    for (auto prof : g_group_order_dirty) {
                        auto it = g_group_order.find(prof);
                        if (it == g_group_order.end()) continue;
                        json arr = json::array();
                        for (auto& id : it->second) arr.push_back(id);
                        orders[std::to_string(prof)] = std::move(arr);
                    }
                    if (!orders.empty())
                        payload["groupOrder"] = std::move(orders);
                    g_group_order_dirty.clear();
                }
            }
            payload["entries"] = json::array();

            double now = now_s();

            {
                std::scoped_lock lk(g_mutex);
                for (auto& e : g_tracked) {
                    if (!e.enabled || e.skillid == 0) continue;

                    float left = compute_left_for(e.skillid, e.base_cd, now);
                    const bool ready =
                        (left >= 0.f && left <= 0.5f) ||
                        (g_by_skill.find(e.skillid) == g_by_skill.end());

                    json row;
                    row["label"] = e.label;
                    row["ready"] = ready;
                    row["left"] = (left < 0 ? nullptr : json(left));
                    row["skillid"] = e.skillid;
                    payload["entries"].push_back(row);
                }
            }

            std::string resp;
            bool ok = http_post_json(
                g_server_host, g_server_port, g_use_https,
                L"/update", payload.dump(), &resp
            );

            if (ok && !resp.empty()) {
                try {
                    auto jr = json::parse(resp);
                    if (jr.contains("assignedName") && jr["assignedName"].is_string()) {
                        g_assigned_name = jr["assignedName"].get<std::string>();
                    }
                }
                catch (const std::exception& e) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "[sqcd] /update JSON error: %s", e.what());
                    arc_log(buf);
                }
            }
        }

        // ---- PULL /aggregate ----
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - last_pull).count() >= 1000) {
            last_pull = now_tp;
            std::wstring qp = L"/aggregate?room=" + std::wstring(g_room.begin(), g_room.end());
            std::string resp;
            bool ok = http_get(g_server_host, g_server_port, g_use_https, qp, &resp);
            if (ok && !resp.empty()) {
                try {
                    auto jr = json::parse(resp);
                    {
                        std::scoped_lock lk(g_mutex);
                        g_cached_raw = jr;
                        parse_peers_from_json_locked(jr);
                        inject_self_if_missing_locked();
                    }

                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                        "[sqcd] aggregate parsed peers=%zu",
                        (size_t)g_peers.size());
                    arc_log(buf);
                }
                catch (const std::exception& e) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "[sqcd] aggregate JSON exception: %s", e.what());
                    arc_log(buf);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}


// -----------------------------------------------------

static void move_peer_up_in_group(uint32_t prof, const std::string& id) {
    std::scoped_lock lk(g_mutex);
    auto& vec = g_group_order[prof];
    for (size_t i = 0; i < vec.size(); ++i) if (vec[i] == id) {
        if (i > 0) {
            std::swap(vec[i - 1], vec[i]);
            save_settings_all();
            g_group_order_dirty.insert(prof);
        }
        return;
    }
}

static void move_peer_down_in_group(uint32_t prof, const std::string& id) {
    std::scoped_lock lk(g_mutex);
    auto& vec = g_group_order[prof];
    for (size_t i = 0; i < vec.size(); ++i) if (vec[i] == id) {
        if (i + 1 < vec.size()) {
            std::swap(vec[i + 1], vec[i]);
            save_settings_all();
            g_group_order_dirty.insert(prof);
        }
        return;
    }
}

static void draw_tracked_ui() {
    static bool  s_prev_open = false;
    static int   s_prev_row_count = 0;
    static float s_collapsed_height = 0.0f;
    static float s_row_step = 0.0f;

    ImVec2 win_size_before = ImGui::GetWindowSize();
    bool is_open = ImGui::CollapsingHeader("Tracked skills", ImGuiTreeNodeFlags_DefaultOpen);

    if (s_row_step <= 0.0f) {
        ImGuiStyle& style = ImGui::GetStyle();
        s_row_step = ImGui::GetTextLineHeightWithSpacing() + style.CellPadding.y;
    }

    if (!s_prev_open && is_open) {
        s_collapsed_height = win_size_before.y;

        int row_count = 0;
        {
            std::scoped_lock lk(g_mutex);
            row_count = (int)g_tracked.size();
        }

        float extra = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        float target_h = s_collapsed_height + s_row_step * row_count + extra;
        ImGui::SetWindowSize(ImVec2(win_size_before.x, target_h));

        s_prev_row_count = row_count;
        s_prev_open = true;
    }
    else if (s_prev_open && !is_open) {
        if (s_collapsed_height > 0.0f) {
            ImGui::SetWindowSize(ImVec2(win_size_before.x, s_collapsed_height));
        }

        s_prev_open = false;
        s_prev_row_count = 0;
        return;
    }

    if (!is_open) {
        s_prev_open = false;
        return;
    }

    std::scoped_lock lk(g_mutex);

    const ImGuiTableFlags tf =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit;

    int erase = -1;
    double now = now_s();

    if (ImGui::BeginTable("##tracked", 4, tf)) {
        ImGui::TableSetupColumn("## ");
        ImGui::TableSetupColumn("##Skill Name");
        ImGui::TableSetupColumn("## ");
        ImGui::TableSetupColumn("##Cooldown");

        for (int i = 0; i < (int)g_tracked.size(); ++i) {
            auto& e = g_tracked[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Checkbox(("##on" + std::to_string(i)).c_str(), &e.enabled);

            ImGui::TableSetColumnIndex(1);
            {
                ImGui::PushItemWidth(220.f);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", e.label.c_str());
                if (ImGui::InputText(("##lbl" + std::to_string(i)).c_str(), buf, IM_ARRAYSIZE(buf))) {
                    e.label = buf;
                    g_label_save_pending = true;
                    g_last_label_edit_s = now_s();
                }
                ImGui::PopItemWidth();
            }

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button(("Delete##" + std::to_string(i)).c_str())) {
                erase = i;
            }

            ImGui::TableSetColumnIndex(3);
            {
                float left = -1.f;
                if (e.skillid) left = compute_left_for(e.skillid, e.base_cd, now);

                if (e.skillid == 0) {
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "no skill");
                }
                else if (g_by_skill.find(e.skillid) == g_by_skill.end()) {
                    ImGui::TextDisabled("");
                }
                else if (left <= 0.5f && left >= 0.f) {
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "READY");
                }
                else if (left >= 0.f) {
                    if (left < 10.0f) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%.1fs", left);
                    }
                    else {
                        ImGui::Text("%.1fs", left);
                    }
                }
                else {
                    ImGui::TextDisabled("waiting");
                }
            }
        }

        if (erase >= 0) {
            g_tracked.erase(g_tracked.begin() + erase);

            if (g_pick_row == erase) {
                g_pick_row = -1;
                g_pick_armed_until_s = 0.0;
            }
            else if (g_pick_row > erase) {
                --g_pick_row;
            }

            save_settings_all();
        }

        ImGui::EndTable();
    }

    if (ImGui::Button("Add tracked skill")) {
        g_tracked.push_back(TrackedEntry{ true, 0, 0.f, "Label" });
        int newIndex = (int)g_tracked.size() - 1;
        g_pick_row = newIndex;
        g_pick_armed_until_s = now_s() + 6.0;
        g_pick_not_before_ms = g_last_ev_ms;
        save_settings_all();
    }

    if (g_label_save_pending && g_last_label_edit_s > 0.0) {
        double tnow = now_s();
        if (tnow - g_last_label_edit_s >= LABEL_SAVE_DELAY_S) {
            save_settings_all();
            g_label_save_pending = false;
        }
    }

    int rows_now = (int)g_tracked.size();
    if (s_prev_row_count == 0) {
        s_prev_row_count = rows_now;
    }
    else if (rows_now != s_prev_row_count) {
        int delta = rows_now - s_prev_row_count;
        ImVec2 cur = ImGui::GetWindowSize();
        float new_h = cur.y + delta * s_row_step;
        ImGui::SetWindowSize(ImVec2(cur.x, new_h));
        s_prev_row_count = rows_now;
    }

    s_prev_open = true;
}

static const ImVec4 SEP_COLOR(0.8f, 0.8f, 0.8f, 0.8f);

static void draw_group_table(uint32_t prof, const std::vector<Peer>& peers_snapshot) {
    std::vector<const Peer*> peers;
    peers.reserve(peers_snapshot.size());
    for (const auto& p : peers_snapshot) {
        if (prof == 0) {
            if (p.prof == 0) peers.push_back(&p);
        }
        else {
            if (p.prof == prof) peers.push_back(&p);
        }
    }
    if (peers.empty())
        return;

    std::vector<std::string> order;
    std::unordered_set<std::string> dead_accounts_snapshot;
    {
        std::scoped_lock lk(g_mutex);
        auto it = g_group_order.find(prof);
        if (it != g_group_order.end())
            order = it->second;

        dead_accounts_snapshot = g_dead_accounts;
    }

    std::unordered_map<std::string, const Peer*> by_id;
    by_id.reserve(peers.size());
    for (const Peer* p : peers) {
        by_id[p->id] = p;
    }

    std::vector<const Peer*> ordered;
    ordered.reserve(peers.size());

    for (const auto& id : order) {
        auto it = by_id.find(id);
        if (it != by_id.end()) {
            ordered.push_back(it->second);
            by_id.erase(it);
        }
    }

    if (!by_id.empty()) {
        std::vector<const Peer*> extra;
        extra.reserve(by_id.size());
        for (auto& kv : by_id) extra.push_back(kv.second);
        std::sort(extra.begin(), extra.end(), [](const Peer* a, const Peer* b) {
            const std::string& an = a->name.empty() ? a->id : a->name;
            const std::string& bn = b->name.empty() ? b->id : b->name;
            if (an != bn) return an < bn;
            return a->id < b->id;
            });
        ordered.insert(ordered.end(), extra.begin(), extra.end());
    }

    ImVec4 base_color = prof_color(prof);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 1.0f));

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit;

    std::string table_id = "squad_prof_" + std::to_string(prof);
    if (ImGui::BeginTable(table_id.c_str(), 4, flags)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Skills", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Re", ImGuiTableColumnFlags_WidthFixed, 60.0f);

        int row = 0;

        for (const Peer* p : ordered) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", ++row);

            bool is_dead = false;
            if (!p->account.empty() && dead_accounts_snapshot.count(p->account))
                is_dead = true;
            else if (!p->name.empty() && dead_accounts_snapshot.count(p->name))
                is_dead = true;

            ImVec4 name_color = base_color;
            if (is_dead) {
                name_color = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
            }

            ImGui::TableSetColumnIndex(1);
            {
                const std::string& display_name = !p->name.empty() ? p->name : p->id;
                ImGui::TextColored(name_color, "%s", display_name.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            if (p->entries.empty()) {
                ImGui::TextDisabled("(no data)");
            }
            else {
                for (size_t i = 0; i < p->entries.size(); ++i) {
                    const PeerEntry& e = p->entries[i];

                    if (i) {
                        ImGui::SameLine(0.0f, 4.0f);
                        ImGui::TextColored(SEP_COLOR, "|");
                        ImGui::SameLine(0.0f, 4.0f);
                    }

                    if (e.ready) {
                        ImGui::TextColored(ImVec4(0.60f, 1.00f, 0.60f, 1.00f), "%s", e.label.c_str());
                    }
                    else if (e.left >= 0.f) {
                        if (e.left < 10.0f) {
                            ImGui::TextColored(
                                ImVec4(1.00f, 0.80f, 0.40f, 1.00f),
                                "%s %.0fs", e.label.c_str(), e.left
                            );
                        }
                        else {
                            ImGui::Text("%s %.0fs", e.label.c_str(), e.left);
                        }
                    }
                    else {
                        ImGui::TextDisabled("%s ?", e.label.c_str());
                    }
                }
            }

            ImGui::TableSetColumnIndex(3);
            const std::string up_id = "up##" + p->id;
            const std::string dn_id = "dn##" + p->id;

            if (ImGui::ArrowButton(up_id.c_str(), ImGuiDir_Up)) {
                move_peer_up_in_group(prof, p->id);
            }
            ImGui::SameLine(0.0f, 2.0f);
            if (ImGui::ArrowButton(dn_id.c_str(), ImGuiDir_Down)) {
                move_peer_down_in_group(prof, p->id);
            }
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    ImGui::Spacing();
}

static void draw_squad_ui() {
    std::vector<Peer> peers_snapshot;
    uint32_t my_subgroup = 0;
    std::unordered_set<std::string> squad_accounts;
    std::string my_account;

    {
        std::scoped_lock lk(g_mutex);
        peers_snapshot = g_peers;
        my_subgroup = g_self.subgroup;
        squad_accounts = g_squad_accounts;
        my_account = g_self_accountname;
    }

    if (peers_snapshot.empty()) {
        ImGui::TextDisabled("No peers yet. Others must run the addon and enable sharing.");
        return;
    }

    std::vector<Peer> filtered;
    filtered.reserve(peers_snapshot.size());

    const bool have_squad_accounts = !squad_accounts.empty();

    if (have_squad_accounts) {
        for (auto& p : peers_snapshot) {
            if (p.id == g_client_id) {
                filtered.push_back(p);
                continue;
            }

            bool in_squad = false;

            if (!p.account.empty() &&
                squad_accounts.find(p.account) != squad_accounts.end()) {
                in_squad = true;
            }
            else if (!p.name.empty() &&
                squad_accounts.find(p.name) != squad_accounts.end()) {
                in_squad = true;
            }
            else if (my_subgroup != 0 && p.subgroup == my_subgroup) {
                in_squad = true;
            }

            if (in_squad)
                filtered.push_back(p);
        }
    }
    else if (my_subgroup != 0) {
        for (auto& p : peers_snapshot) {
            if (p.subgroup != 0)
                filtered.push_back(p);
        }
    }
    else {
        for (auto& p : peers_snapshot) {
            if (p.id == g_client_id) {
                filtered.push_back(p);
                break;
            }
        }
    }

    peers_snapshot.swap(filtered);

    if (peers_snapshot.empty()) {
        if (my_subgroup != 0)
            ImGui::TextDisabled("No peers in your squad.");
        else
            ImGui::TextDisabled("No local data yet.");
        return;
    }

    // Custom profession order:
    // ranger (4), ele (6), mes (7), necro (8),
    // engi (3), war (2), guard (1), thief (5), rev (9)
    static const uint32_t PROF_ORDER[] = {
        4, // Ranger
        6, // Ele
        7, // Mes
        8, // Necro
        3, // Engi
        2, // War
        1, // Guard
        5, // Thief
        9  // Rev
    };

    for (size_t i = 0; i < sizeof(PROF_ORDER) / sizeof(PROF_ORDER[0]); ++i) {
        draw_group_table(PROF_ORDER[i], peers_snapshot);
    }

    // Unknown / prof=0 at the bottom
    draw_group_table(0, peers_snapshot);
}


static void __cdecl on_imgui(uint32_t not_charsel_or_loading, uint32_t) {
    if (!not_charsel_or_loading)
        return;

    // allow options_windows to draw again this frame if Options is open
    g_options_drawn_this_frame = false;

    if (!g_overlay_enabled)
        return;

    ImGui::SetNextWindowBgAlpha(0.8f);

    ImGui::SetNextWindowSize(ImVec2(380.0f, 200.0f), ImGuiCond_FirstUseEver);

    ImGui::SetNextWindowSizeConstraints(
        ImVec2(260.0f, 80.0f),
        ImVec2(1920.0f, 1080.0f)
    );

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    bool open = true;

    if (ImGui::Begin("Squad Cooldowns", &open, flags)) {
        draw_squad_ui();
        draw_tracked_ui();

        ImVec2 win_size = ImGui::GetWindowSize();
        ImVec2 content_min = ImGui::GetWindowContentRegionMin();
        ImVec2 content_max = ImGui::GetWindowContentRegionMax();

        float content_region_height = content_max.y - content_min.y;
        float overhead = win_size.y - content_region_height;

        float cursor_y = ImGui::GetCursorPosY();
        float used_height = cursor_y - content_min.y;
        if (used_height < 0.0f) used_height = 0.0f;

        used_height += ImGui::GetStyle().ItemSpacing.y;

        float target_height = overhead + used_height;
        if (g_settings_dirty.exchange(false, std::memory_order_relaxed)) {
            save_settings_all();
        }

        const float min_h = 80.0f;
        const float max_h = 1080.0f;
        if (target_height < min_h) target_height = min_h;
        if (target_height > max_h) target_height = max_h;

        if (fabsf(win_size.y - target_height) > 1.0f) {
            ImGui::SetWindowSize(ImVec2(win_size.x, target_height));
        }
    }
    ImGui::End();

    if (!open) {
        g_overlay_enabled = false;
        save_settings_all();
    }
}

static uintptr_t __cdecl options_windows(const char* windowname) {
    (void)windowname;

    if (g_options_drawn_this_frame)
        return 0;
    g_options_drawn_this_frame = true;

    if (ImGui::CollapsingHeader("Squad Cooldowns", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Columns(2, "sqcd_ext_cols");
        ImGui::SetColumnWidth(0, 175.0f);

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted("Overlay visible");
        ImGui::PopStyleColor();

        ImGui::NextColumn();

        bool overlay = g_overlay_enabled;
        ImVec4 overlayColor = overlay
            ? ImVec4(0.60f, 1.00f, 0.60f, 1.00f)
            : ImVec4(0.80f, 0.80f, 0.80f, 1.00f);

        ImGui::PushStyleColor(ImGuiCol_Text, overlayColor);
        if (ImGui::Checkbox("Overlay", &overlay)) {
            g_overlay_enabled = overlay;
            save_settings_all();
        }
        ImGui::PopStyleColor();

        ImGui::NextColumn();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.80f, 0.60f, 1.0f));
        ImGui::TextUnformatted("Share cooldowns");
        ImGui::PopStyleColor();

        ImGui::NextColumn();

        bool share = g_share_enabled;
        ImVec4 shareColor = share
            ? ImVec4(0.60f, 1.00f, 0.60f, 1.00f)
            : ImVec4(0.80f, 0.80f, 0.80f, 1.00f);

        ImGui::PushStyleColor(ImGuiCol_Text, shareColor);
        if (ImGui::Checkbox("Share", &share)) {
            g_share_enabled = share;
            save_settings_all();
        }
        ImGui::PopStyleColor();

        ImGui::NextColumn();

        ImGui::Columns(1);
    }

    return 0;
}

static arcdps_exports g_exp{};

static arcdps_exports* mod_init() {
    if (g_initialized) return &g_exp;
    g_initialized = true;

    load_settings_all();
    if (g_client_id.empty()) {
        g_client_id = make_guid();
        save_settings_all();
    }

    g_exp.size = sizeof(arcdps_exports);
    g_exp.sig = PLUGIN_SIG;
    g_exp.imguivers = IMGUI_VERSION_NUM;
    g_exp.out_name = PLUGIN_NAME;
    g_exp.out_build = PLUGIN_VER;

    g_exp.wnd_nofilter = nullptr;
    g_exp.combat = (void*)&on_combat;
    g_exp.imgui = (void*)&on_imgui;
    g_exp.options_tab = nullptr;
    g_exp.combat_local = nullptr;
    g_exp.wnd_filter = nullptr;
    g_exp.options_windows = (void*)&options_windows;

    g_net_alive = true;
    g_net_thread = std::thread(net_loop);

    return &g_exp;
}

static void mod_release() {
    if (!g_initialized) {
        return;
    }
    g_initialized = false;

    g_net_alive = false;
    if (g_net_thread.joinable()) {
        g_net_thread.join();
    }

    save_settings_all();
}

extern "C" __declspec(dllexport)
void* impl_get_init_addr(char* arcversion, void* imguictx, void* id3dptr,
    HINSTANCE arcdll, void* mallocfn, void* freefn, uint32_t d3dver) {
    ImGui::SetCurrentContext((ImGuiContext*)imguictx);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))mallocfn,
        (void(*)(void*, void*))freefn);

    arc_e0 = (wchar_t* (__cdecl*)())GetProcAddress(arcdll, "e0");
    arc_e3 = (void(__cdecl*)(char*))GetProcAddress(arcdll, "e3");

    return (void*)&mod_init;
}

extern "C" __declspec(dllexport)
void* impl_get_release_addr() {
    return (void*)&mod_release;
}
