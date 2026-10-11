#pragma once
// WitcherCraft M4a combat: SkyCraft's Combat.cpp semantics for The Witcher 3.
//
//   W3 -> MC  ActorTable @0x12000 (seqlock): nearby W3 actors, mirrored by SkyCraft's Fabric mod
//             as invisible hittable stand-ins (SkyrimActorEntity). The script gathers them at ~10 Hz
//             and pushes one batched string (SetActors); the host thread publishes it.
//   MC -> W3  McEvent ring @0x17000: kEvHitActor / kEvPlayerDied / kEvExplosion are converted to
//             W3 units here (host thread) and queued for the script (Pop), which applies them with
//             vanilla W3DamageActions.
//   W3 -> MC  kInHurt: the script's damage hook reports what W3 would have taken off Geralt (Hurt);
//             melee / projectile hits go out at once, magic / other damage is batched for 0.5 s
//             (Minecraft's invulnerability frames would eat ticking damage), like SkyCraft.
//
// Ids: W3 has no FormIDs. The script uses CEntity.GetGuidHash() (an int) as the actor id; it is
// sent to Minecraft as ActorRecord::formId and comes back unchanged in kEvHitActor / kInHurt.
//
// Ini [WitcherCraft] in witchercraft_native.ini (all optional):
//   Combat=1                ; 0 = no actor table, events dropped, W3 damage hits Geralt as usual
//   CombatRange=32          ; metres: W3 actors this close get a Minecraft stand-in
//   CombatHitScale=0.05     ; fraction of the target's max health per Minecraft damage point
//                           ; (diamond sword 7 -> 35 % -> a bandit dies in 3 hits)
//   CombatHitType=0         ; 0 = physical damage by weapon (slashing / piercing / bludgeoning,
//                           ;     armour points ignored, % resistances apply); 1 = direct damage
//   CombatHurtScale=1.5     ; Minecraft damage = W3 damage / Geralt's max vitality * 20 * this
//   CombatDeathMode=0       ; Steve died: 0 = Minecraft respawns him where he died (Geralt is left
//                           ; alone), 1 = Geralt dies too (W3 game over, SkyCraft's behaviour)
#include <cstdint>
#include <string>
#include <string_view>

namespace skycraft::proto
{
struct McState;
struct McEvent;
}

namespace wc::combat
{
struct Config
{
    bool enabled = true;
    float range = 32.0f;
    float hitScale = 0.05f;
    int hitType = 0;
    float hurtScale = 1.5f;
    int deathMode = 0;
};
// Reads the ini once (lazily; any thread).
const Config& GetConfig();

// ---- script thread ----------------------------------------------------------------------
// Batched actors, W3 coordinates: "id,x,y,z,heading,width,height,healthFrac,flags,level,name;..."
// (flags = proto::ActorFlags, name last, ';' not allowed in it). "" = no actors (Witcher mode).
// Returns how many records were parsed.
int SetActors(std::string_view text);

// W3 damaged the (hidden) player while Minecraft drives: forward it to Minecraft as kInHurt.
// kind = proto::HurtKind, w3Damage = what W3 would have taken off Geralt's vitality,
// maxHealth = Geralt's max vitality, attacker = actor id (0 = none), flags = proto::HurtFlags.
// Returns the Minecraft damage it will become.
float Hurt(int kind, float w3Damage, float maxHealth, int32_t attacker, uint32_t flags);

// A Minecraft event, converted for W3.
struct Event
{
    uint32_t type = 0;   // proto::McEventType
    int32_t id = 0;      // actor id (hit: the target; died: the killer, 0 = none)
    uint32_t flags = 0;  // hit: proto::HitFlags
    uint32_t weapon = 0; // hit: proto::HitWeapon
    // hit:       x = fraction of the target's max health to take (mcDamage * CombatHitScale),
    //            y = Minecraft damage, z = knockback strength, w = knockback heading (W3 degrees)
    // explosion: x, y, z = centre (W3 coords), w = radius (metres)
    float x = 0, y = 0, z = 0, w = 0;
};
// Next queued event; false if none.
bool Pop(Event& out);

// ---- host thread ------------------------------------------------------------------------
// Every MC event drained from the ring (skycraft_host.cpp DrainEventRing).
void OnMcEvent(const skycraft::proto::McEvent& ev);
// Every host iteration: publishes the actor table when it changed, flushes batched damage.
void HostPump(uint8_t* base, bool mcAlive);

// ---- diagnostics ------------------------------------------------------------------------
struct Stats
{
    uint64_t tablesWritten = 0, actorsPublished = 0;
    uint64_t hits = 0, deaths = 0, explosions = 0, arrows = 0, fires = 0, otherEvents = 0;
    uint64_t hurtsSent = 0;
    float lastHurtMc = 0;
    int32_t lastHitId = 0;
    float lastHitMc = 0;
};
Stats GetStats();
std::string StatusSuffix();
} // namespace wc::combat
