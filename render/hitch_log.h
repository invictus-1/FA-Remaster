// [hitches] The hitch log (the viewer's --hitch-log [ms]): what was loaded or made for the first time while the game
// ran - a model, a texture, a pipeline, a remastered tarmac's material, a beat of the simulation that took long - each
// with the processor's time it took, so that a frame that ran long can be named by what it did. The viewer prints, after
// each frame that has notes or ran longer than its threshold, the frame's parts (CpuTimes) and these notes. Off (the
// default): nothing is noted or timed. Notes may come from any thread (kept under a lock; one made off the main thread
// is marked so); a note made inside another's timing (a model's textures inside the model) is indented under it.
#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace render {

struct HitchLog {
  inline static bool on = false;
  inline static std::thread::id mainThread;  // (set with `on`: the thread the frames are drawn on)
  struct Note {
    std::string what;  // "model ueb0201", "pipeline remastered tarmacs", ...
    double ms = 0;
    int depth = 0;     // how deep inside other timed notes it was made (0: none)
    bool elsewhere = false;  // made off the main thread (the simulation's, a worker's)
  };
  // A timed note: from its making to End (or its destruction). Nothing is built or timed while the log is off.
  class Scope {
   public:
    explicit Scope(const char* kind, std::string_view name = {}) {
      if (!on) return;
      what_ = kind;
      if (!name.empty()) what_.append(" ").append(name);
      start_ = std::chrono::steady_clock::now();
      depth_ = Depth()++;
      live_ = true;
    }
    Scope(const char* kind, const char* name) : Scope(kind, std::string_view(name ? name : "")) {}
    ~Scope() { End(); }
    void Append(const std::string& more) {  // (what it turned out to be: a size, a tier)
      if (live_) what_ += more;
    }
    void End() {
      if (!live_) return;
      live_ = false;
      --Depth();
      Add(std::move(what_), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count(), depth_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    std::string what_;
    std::chrono::steady_clock::time_point start_;
    int depth_ = 0;
    bool live_ = false;
  };
  static void Add(std::string what, double ms, int depth = 0) {
    if (!on) return;
    const bool elsewhere = std::this_thread::get_id() != mainThread;
    std::lock_guard<std::mutex> lock(Mutex());
    Notes().push_back({std::move(what), ms, depth, elsewhere});
  }
  // The notes made since the last call, in the order they ended (a note's parts before it); let go of.
  static std::vector<Note> Take() {
    std::lock_guard<std::mutex> lock(Mutex());
    std::vector<Note> out;
    out.swap(Notes());
    return out;
  }

 private:
  static int& Depth() {
    thread_local int depth = 0;
    return depth;
  }
  static std::mutex& Mutex() {
    static std::mutex m;
    return m;
  }
  static std::vector<Note>& Notes() {
    static std::vector<Note> n;
    return n;
  }
};

}  // namespace render
