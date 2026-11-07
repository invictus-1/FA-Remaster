// The replay timeline (ours, not the original's: Chris's approval of 2026-10-23): a bar over a replay with its length,
// where it is, the game's moments and how much fighting there was, and jumps to any point; and the player's own
// bookmarks on it (Chris, 2026-11-07). The host keeps the replay (app/game_session.cpp: its length, the moments' scan,
// the jumps, the bookmarks); the bar is the interface's own
// (ui/data/lua/timeline.lua, of the game's own controls); the functions between the two are here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "script/lua.hpp"

namespace moho::ui {

// A replay bookmark (ours: Chris, 2026-11-07, "lets do all of those"): the player's own named mark on a replay's
// timeline, at a game tick. Kept with the replay in a small file beside it (ReplayBookmarksFile; the replay file itself
// is never written), read when that replay is opened again.
struct ReplayBookmark {
  uint32_t tick = 0;
  std::string name;
};

class ReplayTimelineSource {
 public:
  virtual ~ReplayTimelineSource() = default;
  struct State {
    uint32_t ticks = 0;      // the replay's length in beats
    uint32_t tick = 0;       // the beat the game is at
    uint32_t seekTo = 0;     // a jump under way: the beat it goes to (0: none)
    bool scanning = false;   // the moments are still being looked for
    uint32_t scanned = 0;    // how far the scan has got (a beat)
    size_t moments = 0;      // moments found so far
    size_t buckets = 0;      // fighting buckets (5 s each) found so far
    bool kept = false;       // what a jump back keeps across the restart is there (TakeKept)
    uint32_t jumpsBack = 0;            // jumps back made to their beat since the replay began
    uint32_t backLoadingScreens = 0;   // loading screens put up during them (none the held way: GameSession::Rewind)
    double lastBackSeconds = 0;        // the last one's time from the jump asked to its beat reached (the PC's clock)
  };
  struct Moment {
    int tick = 0;
    // "CommanderDied", "ExperimentalFinished", "ExperimentalDied", "ArmyDefeated", "NukeLaunched", "NukeLanded",
    // "NukeIntercepted", "GameEnded" (the engine's Moment::Kind, sim/moments.h)
    std::string kind;
    int army = -1;     // 0-based
    float pos[3] = {0, 0, 0};
    std::string unit;
    uint32_t entity = 0xffffffff;  // the unit's or the missile's entity id; 0xffffffff none
  };
  struct Fight {  // every army's fighting in those 5 s, summed (with the hits that have no army)
    int tick0 = 0;
    float damage = 0;
    int hits = 0, deaths = 0;
  };
  virtual State Now() const = 0;
  virtual void MomentsFrom(size_t first, std::vector<Moment>* out) const = 0;
  virtual void FightFrom(size_t first, std::vector<Fight>* out) const = 0;
  // Jump to a beat: forward the replay runs ahead to it; back it starts over and runs ahead. `keep` (the
  // interface's, serialized Lua: its camera) is kept across a jump back, for the new interface (TakeKept). False
  // with the reason when there is nothing to jump to.
  virtual bool Seek(uint32_t tick, const std::string& keep, std::string* why) = 0;
  virtual std::string TakeKept() = 0;
  // The player's bookmarks of this replay, in the order of their beats: read from its file beside it when the replay
  // opened (LoadReplayBookmarks), kept across jumps back. SetBookmarks puts `list` in their place (CleanBookmarks:
  // the names made plain, the ticks within the replay, in order) and writes the file; false with the reason when it
  // cannot be written (they are kept for this run of the replay all the same). BookmarksFile: that file.
  virtual std::vector<ReplayBookmark> Bookmarks() const = 0;
  virtual bool SetBookmarks(const std::vector<ReplayBookmark>& list, std::string* why) = 0;
  virtual std::string BookmarksFile() const = 0;
  // For tests (a jump back lands exactly where a straight playback is): the simulation's state line (the engine's
  // Fingerprint::StateLine, its "lua" part left out) after each of these ticks, taken as the beats run from now on,
  // kept across jumps back; Watch anew forgets the lines taken.
  virtual void Watch(const std::vector<uint32_t>& ticks) = 0;
  virtual std::map<uint32_t, std::string> Watched() const = 0;
  // For tests: the next frame the window shows is saved as a PNG (the host's --window-capture way; nothing without a
  // window's picture).
  virtual void CaptureForTests(const std::string& path) = 0;
};

// The bookmarks' file of a replay: `<replay>.bookmarks.txt` beside it (the replay's whole name kept, so that two
// replays of one name and another extension never share one; the game's Replay dialog lists only its replays').
std::string ReplayBookmarksFile(const std::string& replay);
// Which replay a bookmarks file belongs to: the replay's size in bytes and its FNV-1a 64-bit hash, "<size> <16 hex>"
// (LastGame.SCFAReplay is written anew by every game: the bookmarks of an earlier game are not taken for the next).
std::string ReplayDigest(const std::string& bytes);
// The bookmarks as they are kept: each name on one line without control characters, blanks run together and at most
// 64 bytes (cut at a character's start); an empty name the tick's time ("m:ss", "h:mm:ss"); the ticks at most `ticks`
// (0: no limit); in the order of their ticks (of one tick, in the order given); at most 500.
std::vector<ReplayBookmark> CleanBookmarks(std::vector<ReplayBookmark> list, uint32_t ticks);
// The file's text: a first line naming the format, a comment, "replay <digest>", then one bookmark a line, its tick, a
// tab and its name. ParseReplayBookmarks reads it back: false with the reason for a file that is not one, or is
// another replay's (its replay line's digest is not `digest`); a file without a replay line is taken (made by hand).
std::string FormatReplayBookmarks(const std::string& replayName, const std::string& digest, const std::vector<ReplayBookmark>& list);
bool ParseReplayBookmarks(const std::string& text, const std::string& digest, std::vector<ReplayBookmark>* out, std::string* why);
// Read and write a replay's bookmarks file (`replay`: the replay's own path). Load: true with none when there is no
// file; false with the reason when there is one that cannot be used. Save: nothing written for no bookmarks when there
// is no file yet; false with the reason when it cannot be written.
bool LoadReplayBookmarks(const std::string& replay, const std::string& digest, std::vector<ReplayBookmark>* out, std::string* why);
bool SaveReplayBookmarks(const std::string& replay, const std::string& digest, const std::vector<ReplayBookmark>& list, std::string* why);

// moho64ui_ReplayTimeline, moho64ui_ReplayMoments, moho64ui_ReplayFight, moho64ui_ReplaySeek, moho64ui_ReplayTakeKept,
// moho64ui_ReplayBookmarks, moho64ui_ReplaySetBookmarks, moho64ui_ReplayWatchForTests, moho64ui_ReplayWatchedForTests,
// moho64ui_ReplayCaptureForTests (replay_timeline.cpp): nil where the state has no replay timeline
// (UserState::Config::timeline).
void RegisterTimelineBindings(lua_State* L);
// The console's remaster_timeline [on|off] (the key "Replay timeline", Ctrl-T at first): the bar shown or hidden
// while a replay plays.
void TimelineCommand(lua_State* L, const std::string& value);

}  // namespace moho::ui
