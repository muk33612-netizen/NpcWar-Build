// NpcWar - ScriptHookV (C++ native, no .NET)
// Story-mode mod: 70% gang / 20% police / 10% beggars (allies), tough & aggressive NPCs,
// revive corpses with R, carry corpses with Space.
// Build: see README / build.bat (needs the ScriptHookV SDK headers + ScriptHookV.lib)

#define NOMINMAX
#include <windows.h>
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "natives.h"
#include "types.h"
#include "enums.h"
#include "main.h"

// If your SDK uses the newer namespace names (TASK / MISC) build with /DSHV_NEW_NAMESPACES
#ifdef SHV_NEW_NAMESPACES
namespace AI = TASK;
namespace GAMEPLAY = MISC;
#endif

// ------------------------------------------------------------------ config
struct Config {
    int enabled = 1, total = 50, pctBeggar = 10, pctGang = 70, pctPolice = 20;
    int clearVanilla = 1, noWanted = 1;
    int attackChance = 12, attackRange = 70, mobPercent = 60, meleePercent = 40;
    int enemyHealth = 3000, allyHealth = 4000, accuracy = 60, corpseSeconds = 120;
    int keyRevive = 'R', keyCarry = VK_SPACE;
    float minDist = 35.f, maxDist = 110.f, despawnDist = 170.f;
    float begMin = 8.f, begMax = 30.f, reviveRange = 3.5f;
    int maxAllies = 15;
} cfg;

static HMODULE g_module;

static void loadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy(dot, ".ini");
    auto I = [&](const char* s, const char* k, int d) { return (int)GetPrivateProfileIntA(s, k, d, path); };
    cfg.enabled = I("General", "Enabled", 1);
    cfg.total = I("General", "TotalPeds", 50);
    cfg.pctBeggar = I("General", "BeggarPercent", 10);
    cfg.pctGang = I("General", "GangPercent", 70);
    cfg.pctPolice = I("General", "PolicePercent", 20);
    cfg.clearVanilla = I("General", "ClearVanillaPeds", 1);
    cfg.noWanted = I("General", "DisableWanted", 1);
    cfg.attackChance = I("Combat", "AttackChancePercent", 12);
    cfg.attackRange = I("Combat", "AttackRange", 70);
    cfg.mobPercent = I("Combat", "MobPercent", 60);
    cfg.meleePercent = I("Combat", "MeleePercent", 40);
    cfg.enemyHealth = I("Combat", "EnemyHealth", 3000);
    cfg.allyHealth = I("Combat", "AllyHealth", 4000);
    cfg.accuracy = I("Combat", "Accuracy", 60);
    cfg.corpseSeconds = I("Combat", "CorpseSeconds", 120);
    cfg.keyRevive = I("Keys", "Revive", 'R');
    cfg.keyCarry = I("Keys", "Carry", VK_SPACE);
    cfg.minDist = (float)I("Spawn", "MinDist", 35);
    cfg.maxDist = (float)I("Spawn", "MaxDist", 110);
    cfg.despawnDist = (float)I("Spawn", "DespawnDist", 170);
    cfg.begMin = (float)I("Spawn", "BeggarMinDist", 8);
    cfg.begMax = (float)I("Spawn", "BeggarMaxDist", 30);
    cfg.maxAllies = I("Spawn", "MaxAllies", 15);
    cfg.reviveRange = (float)I("Spawn", "ReviveRange", 35) / 10.f;
}

// ------------------------------------------------------------------ state
enum Role { R_BEGGAR = 0, R_GANG = 1, R_POLICE = 2, R_ALLY = 3 };

struct Npc {
    Ped ped = 0;
    int role = R_GANG;
    bool hostile = false, carried = false, revived = false;
    Hash weapon = 0;
    Ped lastTarget = 0;
    DWORD born = 0, deadAt = 0, nextRoll = 0, nextOrder = 0, ragdollOffAt = 0;
};

static std::vector<Npc> g_npcs;
static std::mt19937 g_rng;
static Hash g_grpAlly, g_grpGang, g_grpPolice;
static std::vector<Hash> g_mBeggar, g_mGang, g_mPolice;
static std::vector<Hash> g_wMelee, g_wGun, g_wCop;
static Hash H_UNARMED;
static int g_carryIdx = -1;   // index into g_npcs of the carried corpse (-1 none)

static int rnd(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_rng); }
static float rndf(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g_rng); }
static Hash hashOf(const char* s) { return GAMEPLAY::GET_HASH_KEY((char*)s); }

static float dist3(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
static Vector3 posOf(Entity e) { return ENTITY::GET_ENTITY_COORDS(e, TRUE); }

// ------------------------------------------------------------------ setup helpers
static void loadModels(const char* const* names, int n, std::vector<Hash>& out) {
    for (int i = 0; i < n; i++) {
        Hash h = hashOf(names[i]);
        if (!STREAMING::IS_MODEL_IN_CDIMAGE(h) || !STREAMING::IS_MODEL_VALID(h)) continue;
        STREAMING::REQUEST_MODEL(h);
        DWORD until = GetTickCount() + 5000;
        while (!STREAMING::HAS_MODEL_LOADED(h) && GetTickCount() < until) WAIT(0);
        if (STREAMING::HAS_MODEL_LOADED(h)) out.push_back(h);
    }
}

static void initAll() {
    g_rng.seed((unsigned)GetTickCount());
    H_UNARMED = hashOf("WEAPON_UNARMED");

    PED::ADD_RELATIONSHIP_GROUP((char*)"NW_ALLY", &g_grpAlly);
    PED::ADD_RELATIONSHIP_GROUP((char*)"NW_GANG", &g_grpGang);
    PED::ADD_RELATIONSHIP_GROUP((char*)"NW_POLICE", &g_grpPolice);
    Hash player = hashOf("PLAYER");
    // 0 companion, 3 neutral, 5 hate
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(0, g_grpAlly, player);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(0, player, g_grpAlly);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, g_grpGang, player);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, player, g_grpGang);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, g_grpPolice, player);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, player, g_grpPolice);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, g_grpGang, g_grpAlly);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, g_grpAlly, g_grpGang);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, g_grpPolice, g_grpAlly);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(5, g_grpAlly, g_grpPolice);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, g_grpGang, g_grpPolice);
    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(3, g_grpPolice, g_grpGang);

    static const char* beg[] = { "a_m_m_tramp_01", "a_m_o_tramp_01", "a_f_m_tramp_01", "a_m_m_trampbeac_01" };
    static const char* gang[] = { "g_m_y_ballasout_01", "g_m_y_ballaeast_01", "g_m_y_ballaorig_01", "g_m_y_famca_01",
        "g_m_y_famdnf_01", "g_m_y_famfor_01", "g_m_y_mexgoon_01", "g_m_y_mexgang_01", "g_m_y_lost_01",
        "g_m_y_korean_01", "g_m_y_azteca_01", "g_m_y_strpunk_01" };
    static const char* cop[] = { "s_m_y_cop_01", "s_f_y_cop_01", "s_m_y_sheriff_01", "s_f_y_sheriff_01", "s_m_y_hwaycop_01" };
    loadModels(beg, 4, g_mBeggar);
    loadModels(gang, 12, g_mGang);
    loadModels(cop, 5, g_mPolice);

    const char* melee[] = { "WEAPON_KNIFE", "WEAPON_BAT", "WEAPON_MACHETE", "WEAPON_CROWBAR", "WEAPON_HAMMER", "WEAPON_GOLFCLUB" };
    const char* gun[] = { "WEAPON_PISTOL", "WEAPON_COMBATPISTOL", "WEAPON_MICROSMG", "WEAPON_SMG", "WEAPON_PUMPSHOTGUN", "WEAPON_ASSAULTRIFLE" };
    const char* copw[] = { "WEAPON_NIGHTSTICK", "WEAPON_COMBATPISTOL", "WEAPON_SMG", "WEAPON_PUMPSHOTGUN", "WEAPON_CARBINERIFLE" };
    for (auto s : melee) g_wMelee.push_back(hashOf(s));
    for (auto s : gun) g_wGun.push_back(hashOf(s));
    for (auto s : copw) g_wCop.push_back(hashOf(s));

    if (cfg.noWanted) {
        PLAYER::SET_MAX_WANTED_LEVEL(0);
    }
}

static Hash pickWeapon(int role) {
    if (role == R_POLICE && rnd(0, 99) < 35) return g_wCop[0];       // nightstick
    if (role == R_POLICE) return g_wCop[rnd(1, (int)g_wCop.size() - 1)];
    if (rnd(0, 99) < cfg.meleePercent) return g_wMelee[rnd(0, (int)g_wMelee.size() - 1)];
    return g_wGun[rnd(0, (int)g_wGun.size() - 1)];
}

static bool isMelee(Hash w) {
    for (Hash m : g_wMelee) if (m == w) return true;
    return w == g_wCop[0];
}

// make a ped tough, fearless, hard to knock down
static void toughen(Ped p, int hp, int acc) {
    PED::SET_PED_MAX_HEALTH(p, hp);
    ENTITY::SET_ENTITY_HEALTH(p, hp);
    PED::SET_PED_ARMOUR(p, 100);
    PED::SET_PED_CAN_RAGDOLL(p, FALSE);
    PED::SET_PED_CAN_RAGDOLL_FROM_PLAYER_IMPACT(p, FALSE);
    PED::SET_PED_SUFFERS_CRITICAL_HITS(p, FALSE);
    WEAPON::SET_PED_DROPS_WEAPONS_WHEN_DEAD(p, FALSE);
    PED::SET_PED_CAN_BE_KNOCKED_OFF_VEHICLE(p, 1);
    PED::SET_PED_COMBAT_ABILITY(p, 2);
    PED::SET_PED_COMBAT_MOVEMENT(p, 2);
    PED::SET_PED_ACCURACY(p, acc);
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 5, TRUE);    // fight armed peds when unarmed
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 46, TRUE);   // always fight
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 58, TRUE);   // disable flee from combat
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 17, FALSE);  // never always-flee
    PED::SET_PED_COMBAT_ATTRIBUTES(p, 0, TRUE);    // use cover
    PED::SET_PED_FLEE_ATTRIBUTES(p, 0, FALSE);
}

static void giveLoadout(Npc& n) {
    WEAPON::REMOVE_ALL_PED_WEAPONS(n.ped, TRUE);
    n.weapon = pickWeapon(n.role == R_POLICE ? R_POLICE : R_GANG);
    WEAPON::GIVE_WEAPON_TO_PED(n.ped, n.weapon, 9999, FALSE, TRUE);
    if (!isMelee(n.weapon)) {
        Hash mel = g_wMelee[rnd(0, (int)g_wMelee.size() - 1)];
        WEAPON::GIVE_WEAPON_TO_PED(n.ped, mel, 1, FALSE, FALSE);   // backup melee
    }
}

static int countRole(int role, bool aliveOnly = true) {
    int c = 0;
    for (auto& n : g_npcs) if (n.role == role && (!aliveOnly || n.deadAt == 0)) c++;
    return c;
}

// ------------------------------------------------------------------ spawn
static bool spawnNpc(int role) {
    std::vector<Hash>* models = role == R_BEGGAR ? &g_mBeggar : role == R_GANG ? &g_mGang : &g_mPolice;
    if (models->empty()) return false;

    Ped pp = PLAYER::PLAYER_PED_ID();
    Vector3 pc = posOf(pp);
    Vector3 out; bool ok = false;
    for (int t = 0; t < 8 && !ok; t++) {
        float ang = rndf(0.f, 6.2831853f);
        float d = role == R_BEGGAR ? rndf(cfg.begMin, cfg.begMax) : rndf(cfg.minDist, cfg.maxDist);
        float x = pc.x + std::cos(ang) * d, y = pc.y + std::sin(ang) * d, gz = 0.f;
        if (!GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(x, y, pc.z + 50.f, &gz, FALSE)) continue;
        if (!PATHFIND::GET_SAFE_COORD_FOR_PED(x, y, gz, TRUE, &out, 16)) continue;
        if (role != R_BEGGAR && d < 70.f) {
            Vector3 fw = ENTITY::GET_ENTITY_FORWARD_VECTOR(pp);       // don't pop in front of the player
            float dx = out.x - pc.x, dy = out.y - pc.y, len = std::sqrt(dx * dx + dy * dy);
            if (len > 0.01f && (dx * fw.x + dy * fw.y) / len > 0.3f) continue;
        }
        ok = true;
    }
    if (!ok) return false;

    Hash model = (*models)[rnd(0, (int)models->size() - 1)];
    Ped p = PED::CREATE_PED(4, model, out.x, out.y, out.z, rndf(0.f, 360.f), FALSE, FALSE);
    if (!p) return false;
    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);

    Npc n; n.ped = p; n.role = role; n.born = GetTickCount();
    n.nextRoll = n.born + rnd(1500, 6000);
    PED::SET_PED_RELATIONSHIP_GROUP_HASH(p, role == R_BEGGAR ? g_grpAlly : role == R_GANG ? g_grpGang : g_grpPolice);
    toughen(p, role == R_BEGGAR ? cfg.allyHealth : cfg.enemyHealth, cfg.accuracy);
    giveLoadout(n);
    WEAPON::SET_CURRENT_PED_WEAPON(p, role == R_BEGGAR ? n.weapon : H_UNARMED, TRUE);
    if (role == R_BEGGAR) {
        AI::TASK_FOLLOW_TO_OFFSET_OF_ENTITY(p, pp, rndf(-2.f, 2.f), rndf(-3.f, -1.f), 0.f, 2.5f, -1, 3.f, TRUE);
    } else {
        AI::TASK_WANDER_STANDARD(p, 10.f, 10);
    }
    g_npcs.push_back(n);
    return true;
}

static void removeNpc(size_t i) {
    Ped p = g_npcs[i].ped;
    if (ENTITY::DOES_ENTITY_EXIST(p)) {
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);
        PED::DELETE_PED(&p);
    }
    if (g_carryIdx == (int)i) g_carryIdx = -1;
    else if (g_carryIdx > (int)i) g_carryIdx--;
    g_npcs.erase(g_npcs.begin() + i);
}

// ------------------------------------------------------------------ AI
static void makeHostile(Npc& n, bool mob) {
    if (n.role != R_GANG && n.role != R_POLICE) return;
    Ped pp = PLAYER::PLAYER_PED_ID();
    if (!n.hostile) {
        n.hostile = true;
        n.nextOrder = 0;
        WEAPON::SET_CURRENT_PED_WEAPON(n.ped, n.weapon, TRUE);
        PED::SET_PED_COMBAT_RANGE(n.ped, isMelee(n.weapon) ? 0 : 1);
    }
    if (mob) {
        Vector3 c = posOf(n.ped);
        for (auto& o : g_npcs) {
            if (&o == &n || o.hostile || o.deadAt) continue;
            if (o.role != R_GANG && o.role != R_POLICE) continue;
            if (dist3(c, posOf(o.ped)) < 30.f && rnd(0, 99) < cfg.mobPercent) makeHostile(o, false);
        }
    }
    (void)pp;
}

static Npc* nearestEnemyTo(Entity e, float maxD) {
    Vector3 c = posOf(e); Npc* best = nullptr; float bd = maxD;
    Vector3 pc = posOf(PLAYER::PLAYER_PED_ID());
    for (auto& n : g_npcs) {
        if (n.deadAt || (n.role != R_GANG && n.role != R_POLICE)) continue;
        Vector3 p = posOf(n.ped);
        bool relevant = n.hostile || dist3(p, pc) < 25.f || dist3(p, c) < 15.f;
        if (!relevant) continue;
        float d = dist3(c, p);
        if (d < bd) { bd = d; best = &n; }
    }
    return best;
}

static void updateNpc(Npc& n, DWORD now, const Vector3& pc, Ped pp) {
    Ped p = n.ped;
    if (n.ragdollOffAt && now >= n.ragdollOffAt) {
        PED::SET_PED_CAN_RAGDOLL(p, FALSE);
        n.ragdollOffAt = 0;
    }
    Vector3 c = posOf(p);
    float dp = dist3(c, pc);

    if (n.role == R_GANG || n.role == R_POLICE) {
        if (ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, pp, TRUE)) {
            ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(p);
            makeHostile(n, true);
        }
        if (!n.hostile && now >= n.nextRoll) {
            n.nextRoll = now + rnd(3000, 8000);
            if (dp < (float)cfg.attackRange && rnd(0, 99) < cfg.attackChance) makeHostile(n, true);
        }
        if (n.hostile) {
            if (now >= n.nextOrder) {
                n.nextOrder = now + 2000;
                if (!PED::IS_PED_IN_COMBAT(p, pp)) AI::TASK_COMBAT_PED(p, pp, 0, 16);
            }
        } else if (now >= n.nextOrder) {
            n.nextOrder = now + 4000;
            if ((float)ENTITY::GET_ENTITY_SPEED(p) < 0.2f && !PED::IS_PED_IN_MELEE_COMBAT(p) && !PED::IS_PED_SHOOTING(p))
                AI::TASK_WANDER_STANDARD(p, 10.f, 10);
        }
    } else { // allies (beggars + revived)
        if (now < n.nextOrder) return;
        n.nextOrder = now + 1500;
        if (dp > 70.f) {   // lost us -> pop next to the player
            ENTITY::SET_ENTITY_COORDS(p, pc.x + rndf(-3.f, 3.f), pc.y + rndf(-3.f, 3.f), pc.z, FALSE, FALSE, FALSE, TRUE);
        }
        Npc* e = nearestEnemyTo(p, 40.f);
        if (e) {
            if (e->ped != n.lastTarget || !PED::IS_PED_IN_COMBAT(p, e->ped)) {
                n.lastTarget = e->ped;
                WEAPON::SET_CURRENT_PED_WEAPON(p, n.weapon, TRUE);
                AI::TASK_COMBAT_PED(p, e->ped, 0, 16);
            }
        } else {
            n.lastTarget = 0;
            if (dp > 6.f && !PED::IS_PED_IN_MELEE_COMBAT(p) && !PED::IS_PED_SHOOTING(p))
                AI::TASK_FOLLOW_TO_OFFSET_OF_ENTITY(p, pp, rndf(-2.f, 2.f), rndf(-3.f, -1.f), 0.f, dp > 20.f ? 3.f : 2.f, -1, 3.f, TRUE);
        }
    }
}

// ------------------------------------------------------------------ revive / carry
static int nearestCorpse(float range) {
    Vector3 pc = posOf(PLAYER::PLAYER_PED_ID()); int best = -1; float bd = range;
    for (size_t i = 0; i < g_npcs.size(); i++) {
        if (!g_npcs[i].deadAt) continue;
        float d = dist3(pc, posOf(g_npcs[i].ped));
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}

static void dropCarried() {
    if (g_carryIdx < 0 || g_carryIdx >= (int)g_npcs.size()) { g_carryIdx = -1; return; }
    Npc& n = g_npcs[g_carryIdx];
    if (ENTITY::DOES_ENTITY_EXIST(n.ped) && ENTITY::IS_ENTITY_ATTACHED(n.ped)) ENTITY::DETACH_ENTITY(n.ped, TRUE, TRUE);
    n.carried = false;
    AI::CLEAR_PED_TASKS(PLAYER::PLAYER_PED_ID());
    g_carryIdx = -1;
}

static void reviveNpc(Npc& n) {
    if (cfg.maxAllies > 0) {
        int allies = countRole(R_BEGGAR) + countRole(R_ALLY);
        if (n.role != R_BEGGAR && allies >= cfg.maxAllies) return;
    }
    Ped p = n.ped;
    if (ENTITY::IS_ENTITY_ATTACHED(p)) ENTITY::DETACH_ENTITY(p, TRUE, TRUE);
    n.carried = false;
    PED::RESURRECT_PED(p);
    AI::CLEAR_PED_TASKS_IMMEDIATELY(p);
    PED::SET_PED_CAN_RAGDOLL(p, TRUE);           // so it can get up
    n.ragdollOffAt = GetTickCount() + 3000;
    if (n.role != R_BEGGAR) n.role = R_ALLY;     // revived cops / gang switch to our side
    n.hostile = false; n.deadAt = 0; n.revived = true; n.lastTarget = 0;
    PED::SET_PED_RELATIONSHIP_GROUP_HASH(p, g_grpAlly);
    toughen(p, cfg.allyHealth, cfg.accuracy);
    PED::SET_PED_CAN_RAGDOLL(p, TRUE);
    giveLoadout(n);
    PED::SET_PED_TO_RAGDOLL(p, 1500, 1500, 0, FALSE, FALSE, FALSE);
    n.nextOrder = GetTickCount() + 2500;
}

static void pickUp(int idx) {
    Npc& n = g_npcs[idx];
    Ped pp = PLAYER::PLAYER_PED_ID();
    STREAMING::REQUEST_ANIM_DICT((char*)"missfinale_c2mcs_1");
    STREAMING::REQUEST_ANIM_DICT((char*)"nm");
    DWORD until = GetTickCount() + 1500;
    while ((!STREAMING::HAS_ANIM_DICT_LOADED((char*)"missfinale_c2mcs_1") || !STREAMING::HAS_ANIM_DICT_LOADED((char*)"nm")) && GetTickCount() < until) WAIT(0);
    ENTITY::ATTACH_ENTITY_TO_ENTITY(n.ped, pp, 0, 0.27f, 0.15f, 0.63f, 0.5f, 0.5f, 0.0f, FALSE, FALSE, FALSE, FALSE, 2, FALSE);
    AI::TASK_PLAY_ANIM(pp, (char*)"missfinale_c2mcs_1", (char*)"fin_c2_mcs_1_camman", 8.f, -8.f, -1, 49, 0.f, FALSE, FALSE, FALSE);
    AI::TASK_PLAY_ANIM(n.ped, (char*)"nm", (char*)"firemans_carry", 8.f, -8.f, -1, 33, 0.f, FALSE, FALSE, FALSE);
    n.carried = true;
    g_carryIdx = idx;
}

static bool g_prevRev = false, g_prevCarry = false;

static void handleKeys() {
    if (GetForegroundWindow() != FindWindowA("grcWindow", NULL)) return;
    bool r = (GetAsyncKeyState(cfg.keyRevive) & 0x8000) != 0;
    bool c = (GetAsyncKeyState(cfg.keyCarry) & 0x8000) != 0;
    bool rp = r && !g_prevRev, cp = c && !g_prevCarry;
    g_prevRev = r; g_prevCarry = c;

    Ped pp = PLAYER::PLAYER_PED_ID();
    if (ENTITY::IS_ENTITY_DEAD(pp)) return;

    if (rp) {
        int idx = g_carryIdx >= 0 ? g_carryIdx : nearestCorpse(cfg.reviveRange);
        if (idx >= 0) {
            if (idx == g_carryIdx) dropCarried();
            reviveNpc(g_npcs[idx]);
        }
    }
    if (cp) {
        if (g_carryIdx >= 0) dropCarried();
        else if (!PED::IS_PED_IN_ANY_VEHICLE(pp, FALSE)) {
            int idx = nearestCorpse(2.0f);
            if (idx >= 0) pickUp(idx);
        }
    }
}

// ------------------------------------------------------------------ manager
static bool isOurs(Ped p) {
    for (auto& n : g_npcs) if (n.ped == p) return true;
    return false;
}

static void clearVanilla(Ped pp) {
    int arr[256];
    int cnt = worldGetAllPeds(arr, 256);
    for (int i = 0; i < cnt; i++) {
        Ped p = arr[i];
        if (p == pp || !ENTITY::DOES_ENTITY_EXIST(p) || isOurs(p)) continue;
        if (PED::IS_PED_A_PLAYER(p) || (int)PED::GET_PED_TYPE(p) <= 3) continue;
        if (PED::IS_PED_IN_ANY_VEHICLE(p, TRUE) || PED::IS_PED_IN_GROUP(p)) continue;
        if (ENTITY::IS_ENTITY_A_MISSION_ENTITY(p)) continue;
        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, TRUE, TRUE);
        PED::DELETE_PED(&p);
    }
}

static DWORD g_lastManage = 0, g_lastClear = 0;

static void manage() {
    DWORD now = GetTickCount();
    if (now - g_lastManage < 250) return;
    g_lastManage = now;

    Ped pp = PLAYER::PLAYER_PED_ID();
    if (ENTITY::IS_ENTITY_DEAD(pp)) { if (g_carryIdx >= 0) dropCarried(); return; }
    if (g_carryIdx >= 0 && PED::IS_PED_IN_ANY_VEHICLE(pp, FALSE)) dropCarried();
    Vector3 pc = posOf(pp);

    for (size_t i = 0; i < g_npcs.size();) {
        Npc& n = g_npcs[i];
        if (!ENTITY::DOES_ENTITY_EXIST(n.ped)) {
            if (g_carryIdx == (int)i) g_carryIdx = -1; else if (g_carryIdx > (int)i) g_carryIdx--;
            g_npcs.erase(g_npcs.begin() + i); continue;
        }
        if (ENTITY::IS_ENTITY_DEAD(n.ped)) {
            if (!n.deadAt) { n.deadAt = now; n.hostile = false; }
            if (!n.carried && now - n.deadAt > (DWORD)cfg.corpseSeconds * 1000) { removeNpc(i); continue; }
            i++; continue;
        }
        if (n.deadAt) n.deadAt = 0;   // revived by something else
        if (!n.carried && dist3(posOf(n.ped), pc) > cfg.despawnDist && n.role != R_ALLY && n.role != R_BEGGAR) { removeNpc(i); continue; }
        updateNpc(n, now, pc, pp);
        i++;
    }

    // keep the crowd topped up according to the percentages
    int sum = std::max(1, cfg.pctBeggar + cfg.pctGang + cfg.pctPolice);
    int want[3] = { cfg.total * cfg.pctBeggar / sum, cfg.total * cfg.pctGang / sum, cfg.total * cfg.pctPolice / sum };
    int have[3] = { countRole(R_BEGGAR), countRole(R_GANG), countRole(R_POLICE) };
    for (int k = 0; k < 2; k++) {
        int bestRole = -1, bestDef = 0;
        for (int r = 0; r < 3; r++) if (want[r] - have[r] > bestDef) { bestDef = want[r] - have[r]; bestRole = r; }
        if (bestRole < 0) break;
        if (spawnNpc(bestRole)) have[bestRole]++; else break;
    }

    if (cfg.clearVanilla && now - g_lastClear > 3000) { g_lastClear = now; clearVanilla(pp); }
}

// ------------------------------------------------------------------ entry
void ScriptMain() {
    loadConfig();
    if (!cfg.enabled) return;
    WAIT(3000);
    initAll();
    while (true) {
        WAIT(0);
        if (cfg.clearVanilla) {
            PED::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f);
            PED::SET_SCENARIO_PED_DENSITY_MULTIPLIER_THIS_FRAME(0.0f, 0.0f);
        }
        if (cfg.noWanted) PLAYER::SET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID(), 0, FALSE);
        handleKeys();
        manage();
    }
}

BOOL APIENTRY DllMain(HMODULE hInstance, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: g_module = hInstance; scriptRegister(hInstance, ScriptMain); break;
    case DLL_PROCESS_DETACH: scriptUnregister(hInstance); break;
    }
    return TRUE;
}
