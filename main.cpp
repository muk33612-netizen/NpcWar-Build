// NpcWar LITE - ScriptHookV (C++): every nearby pedestrian gets a weapon, huge health,
// can't be ragdolled, never flees, and attacks the player. No spawning, no deleting.
#define NOMINMAX
#include <windows.h>
#include <map>
#include <random>
#include <cmath>
#include <cstring>

#include "natives.h"
#include "types.h"
#include "enums.h"
#include "main.h"

#ifdef SHV_NEW_NAMESPACES
namespace AI = TASK;
namespace GAMEPLAY = MISC;
#endif

static HMODULE g_module;
static int cHealth = 3000, cAttackPct = 100, cRange = 80, cMelee = 40, cWanted = 1, cAcc = 60;
static std::mt19937 g_rng;

struct St { bool armed = false; bool hostile = false; DWORD nextOrder = 0; Hash weapon = 0; };
static std::map<int, St> g_peds;
static Hash g_melee[6], g_gun[6];

static int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_rng); }
static Hash H(const char* s) { return GAMEPLAY::GET_HASH_KEY((char*)s); }

static void loadCfg() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char* dot = strrchr(path, '.'); if (dot) strcpy(dot, ".ini");
    cHealth = GetPrivateProfileIntA("Combat", "EnemyHealth", 3000, path);
    cAttackPct = GetPrivateProfileIntA("Combat", "AttackPercent", 100, path);
    cRange = GetPrivateProfileIntA("Combat", "AttackRange", 80, path);
    cMelee = GetPrivateProfileIntA("Combat", "MeleePercent", 40, path);
    cAcc = GetPrivateProfileIntA("Combat", "Accuracy", 60, path);
    cWanted = GetPrivateProfileIntA("General", "DisableWanted", 1, path);
}

static bool isMelee(Hash w) { for (Hash m : g_melee) if (m == w) return true; return false; }

static void arm(Ped p, St& s) {
    PED::SET_PED_MAX_HEALTH(p, cHealth);
    ENTITY::SET_ENTITY_HEALTH(p, cHealth);
    PED::SET_PED_ARMOUR(p, 100);
    PED::SET_PED_CAN_RAGDOLL(p, FALSE);
    PED::SET_PED_SUFFERS_CRITICAL_HITS(p, FALSE);
    PED::SET_PED_COMBAT_ABILITY(p, 2);
    PED::SET_PED_ACCURACY(p, cAcc);
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 5, TRUE);
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 46, TRUE);
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 58, TRUE);
    PED::SET_PED_FLEE_ATTRIBUTES(p, 0, FALSE);
    s.weapon = (rnd(0, 99) < cMelee) ? g_melee[rnd(0, 5)] : g_gun[rnd(0, 5)];
    WEAPON::GIVE_WEAPON_TO_PED(p, s.weapon, 9999, TRUE, TRUE);
    s.armed = true;
}

static void tick() {
    static DWORD last = 0, lastPrune = 0;
    DWORD now = GetTickCount();
    if (now - last < 300) return;
    last = now;

    Ped pp = PLAYER::PLAYER_PED_ID();
    if (ENTITY::IS_ENTITY_DEAD(pp)) return;
    Vector3 pc = ENTITY::GET_ENTITY_COORDS(pp, TRUE);

    int arr[256];
    int n = worldGetAllPeds(arr, 256);
    for (int i = 0; i < n; i++) {
        Ped p = arr[i];
        if (p == pp || !ENTITY::DOES_ENTITY_EXIST(p) || ENTITY::IS_ENTITY_DEAD(p)) continue;
        if (PED::IS_PED_A_PLAYER(p) || (int)PED::GET_PED_TYPE(p) <= 3) continue;
        if (PED::IS_PED_IN_ANY_VEHICLE(p, TRUE) || PED::IS_PED_IN_GROUP(p)) continue;
        if (ENTITY::IS_ENTITY_A_MISSION_ENTITY(p)) continue;

        Vector3 c = ENTITY::GET_ENTITY_COORDS(p, TRUE);
        float dx = c.x - pc.x, dy = c.y - pc.y, dz = c.z - pc.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > 130.f) continue;

        St& s = g_peds[p];
        if (!s.armed) arm(p, s);

        if (!s.hostile) {
            bool hit = ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, pp, TRUE);
            if (hit) ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(p);
            if (hit || (d < (float)cRange && rnd(0, 99) < cAttackPct)) {
                s.hostile = true;
                WEAPON::SET_CURRENT_PED_WEAPON(p, s.weapon, TRUE);
                PED::SET_PED_COMBAT_MOVEMENT(p, 2);
                PED::SET_PED_COMBAT_RANGE(p, isMelee(s.weapon) ? 0 : 1);
                PED::SET_PED_KEEP_TASK(p, TRUE);
                s.nextOrder = 0;
            }
        }
        if (s.hostile && now >= s.nextOrder) {
            s.nextOrder = now + 2000;
            if (!PED::IS_PED_IN_COMBAT(p, pp)) AI::TASK_COMBAT_PED(p, pp, 0, 16);
        }
    }

    if (now - lastPrune > 10000) {
        lastPrune = now;
        for (auto it = g_peds.begin(); it != g_peds.end();) {
            if (!ENTITY::DOES_ENTITY_EXIST(it->first) || ENTITY::IS_ENTITY_DEAD(it->first)) it = g_peds.erase(it);
            else ++it;
        }
    }
}

void ScriptMain() {
    g_rng.seed((unsigned)GetTickCount());
    loadCfg();
    const char* m[6] = { "WEAPON_KNIFE", "WEAPON_BAT", "WEAPON_MACHETE", "WEAPON_CROWBAR", "WEAPON_HAMMER", "WEAPON_GOLFCLUB" };
    const char* g[6] = { "WEAPON_PISTOL", "WEAPON_COMBATPISTOL", "WEAPON_MICROSMG", "WEAPON_SMG", "WEAPON_PUMPSHOTGUN", "WEAPON_ASSAULTRIFLE" };
    WAIT(2000);
    for (int i = 0; i < 6; i++) { g_melee[i] = H(m[i]); g_gun[i] = H(g[i]); }
    while (true) {
        WAIT(0);
        if (cWanted) PLAYER::SET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID(), 0, FALSE);
        tick();
    }
}

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: g_module = hInstance; scriptRegister(hInstance, ScriptMain); break;
    case DLL_PROCESS_DETACH: scriptUnregister(hInstance); break;
    }
    return TRUE;
}
