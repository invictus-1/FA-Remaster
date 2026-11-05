#include "sim/effect_aliases.h"

namespace moho {

const std::vector<std::pair<std::string, std::string>>& EffectAliases() {
  // Chris, 2026-11-04: each use of /effects/emitters/small_test_fire.bp gets a fireball of its own. A "use" is one
  // table of EffectTemplates.lua that holds the file (ten distinct tables, found in a session's Lua; two of them are
  // also known by a second name, noted beside them). Who plays each:
  static const std::vector<std::pair<std::string, std::string>> kAliases = {
      // ExplosionSmall: small land unit deaths (defaultexplosions.lua)
      {"/effects/emitters/remaster/small_land_death_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // ExplosionSmallAir: air unit deaths (defaultexplosions.lua), Cybran build bots (CBuildBotUnit.lua)
      {"/effects/emitters/remaster/small_air_death_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // FireCloudMed01: hit explosions (defaultexplosions.lua)
      {"/effects/emitters/remaster/medium_fire_cloud_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // FireCloudSml01: the UEF phalanx shell's miss (terranprojectiles.lua, kirvesprojectiles.lua)
      {"/effects/emitters/remaster/small_fire_cloud_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // ExplosionEffectsSml01 (table.concatenate of FireCloudSml01 and others)
      {"/effects/emitters/remaster/explosion_effects_sml01_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // ExplosionEffectsMed01, also ExplosionEffectsDefault01 (of FireCloudMed01)
      {"/effects/emitters/remaster/explosion_effects_med01_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // DefaultHitExplosion01 (of FireCloudMed01; defaultexplosions.lua)
      {"/effects/emitters/remaster/default_hit_explosion01_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // DefaultHitExplosion02 (of FireCloudSml01)
      {"/effects/emitters/remaster/default_hit_explosion02_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // DefaultMissileHit01, also CMissileHit01: Cybran missile hits (cybranprojectiles.lua)
      {"/effects/emitters/remaster/default_missile_hit01_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
      // CMissileHit02: Cybran missile hits (cybranprojectiles.lua)
      {"/effects/emitters/remaster/cybran_missile_hit02_fire_01_emit.bp", "/effects/emitters/small_test_fire.bp"},
  };
  return kAliases;
}

}  // namespace moho
