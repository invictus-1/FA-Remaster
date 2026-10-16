// The visuals track's remastered effects: emitter, beam and trail blueprints and their textures, laid
// out below one folder with the same paths as the game's (visuals/work/_effects/export/moho64), and
// mounted at /remastered by MountRemasters. In the remastered look a file is read from there when the
// visuals track has made one, and from the game otherwise; the original look always reads the game's.
//
// Most of the game's effects are shared by the four factions (the explosions every unit dies with, the
// sparks of a hit), while the remastered look gives each faction its own. So an emitter, beam or trail
// made on or at something of a faction is read first from the faction's folder beside the shared file
// ("/effects/emitters/<faction>/x_emit.bp"), then as above. Whose a thing is, is said below.
#pragma once

#include <string>

#include "core/vfs.h"

namespace render {

inline constexpr const char* kRemasteredEffects = "/remastered";

// The path to read for `path` (as the game names it): the remastered file of that path when
// `remastered` and one exists, `path` itself otherwise.
inline std::string EffectFile(const moho::Vfs& vfs, const std::string& path, bool remastered) {
  if (!remastered || path.empty()) return path;
  std::string ours = std::string(kRemasteredEffects) + moho::Vfs::Normalize(path);
  return vfs.Exists(ours) ? ours : path;
}

// Where the visuals track keeps a faction's own version of a file the game shares: in the faction's
// folder beside it ("/remastered/effects/emitters/uef/x_emit.bp" for "/effects/emitters/x_emit.bp").
inline std::string FactionEffectFile(const std::string& path, const std::string& faction) {
  const std::string n = moho::Vfs::Normalize(path);
  const size_t slash = n.rfind('/');
  if (slash == std::string::npos) return std::string(kRemasteredEffects) + "/" + faction + "/" + n;
  return std::string(kRemasteredEffects) + n.substr(0, slash + 1) + faction + n.substr(slash);
}

// Whose a thing is, for the faction's own effects: "uef", "aeon", "cybran", "seraphim", or "" (not known).

// A unit, by its blueprint id: the game's own ids start with u (the first game), x (Forged Alliance),
// d or z (FAF's) and the faction's letter after it, e for the UEF, a Aeon, r Cybran, s Seraphim
// ("uel0201", "xsl0001", "del0204", "zab9501"). (Checked against General.FactionName of every unit in
// FAF's units.nx2: they agree but for the campaign's civilian buildings, whose looks are the letter's
// faction and whose blueprints all say UEF. Mods' units that name themselves otherwise are not known.)
inline std::string UnitFaction(const std::string& id) {
  if (id.size() < 2) return std::string();
  const char a = static_cast<char>(id[0] | 0x20), b = static_cast<char>(id[1] | 0x20);
  if (a != 'u' && a != 'x' && a != 'd' && a != 'z') return std::string();
  if (b == 'e') return "uef";
  if (b == 'a') return "aeon";
  if (b == 'r') return "cybran";
  if (b == 's') return "seraphim";
  return std::string();
}

// A projectile or an effect entity, by its name (the folder its blueprint is in): the game's weapons
// name the faction first and the kind of weapon after it - T for the UEF ("Terran"), A Aeon, C Cybran,
// S Seraphim; then DF direct fire, IF indirect fire, AA anti-air, AN anti-navy, IM interceptors, BO
// bombs, DP the Phalanx (TDFGauss01, SIFInainoStrategicMissile01, SBOOhwalliBombEffect01). Others say
// it in a word (ShellTankTerran01, TorpedoShipAeon01, UEFNukeEffect01, SeraphimNukeEffectController01).
// (Checked against the base module every projectile script of FAF's projectiles.nx2 builds on -
// terranprojectiles.lua, aeonprojectiles.lua, ...: no disagreement; five are not known by their names,
// FireSparks, Kamikaze01, X1Mercy_proj, aaphalanx01, aaserpentine, and neither are the shared ones,
// the debris, ShieldCollider, Sinker.)
inline std::string NameFaction(const std::string& name) {
  std::string n;
  for (char c : name) n += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  if (n.size() >= 3) {
    const std::string kind = n.substr(1, 2);
    if (kind == "df" || kind == "if" || kind == "aa" || kind == "an" || kind == "im" || kind == "bo" || kind == "dp") {
      if (n[0] == 't') return "uef";
      if (n[0] == 'a') return "aeon";
      if (n[0] == 'c') return "cybran";
      if (n[0] == 's') return "seraphim";
    }
  }
  if (n.find("uef") != std::string::npos || n.find("terran") != std::string::npos) return "uef";
  if (n.find("aeon") != std::string::npos) return "aeon";
  if (n.find("cybran") != std::string::npos) return "cybran";
  if (n.find("seraphim") != std::string::npos) return "seraphim";
  return std::string();
}

// The same by a blueprint's path: the name of the folder it is in ("/projectiles/TDFGauss01/TDFGauss01_proj.bp").
inline std::string PathFaction(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos || slash == 0) return NameFaction(path);
  const size_t before = path.find_last_of("/\\", slash - 1);
  return NameFaction(path.substr(before == std::string::npos ? 0 : before + 1, slash - (before == std::string::npos ? 0 : before + 1)));
}

// A mesh a script set on something (a shield's bubble): by the dome's folder, or the unit whose own
// it is (a commander's personal shield).
inline std::string MeshFaction(const std::string& mesh) {
  const std::string m = moho::Vfs::Normalize(mesh);
  const size_t units = m.find("/units/");
  if (units != std::string::npos) {
    const std::string unit = UnitFaction(m.substr(units + 7));
    if (!unit.empty()) return unit;
  }
  if (m.find("aeonshield") != std::string::npos) return "aeon";
  if (m.find("cybranshield") != std::string::npos) return "cybran";
  if (m.find("seraphimshield") != std::string::npos) return "seraphim";
  if (m.find("/shield01/") != std::string::npos) return "uef";
  return std::string();
}

}  // namespace render
