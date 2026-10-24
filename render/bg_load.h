// [bgload] A world opened on a thread of its own while another world goes on being drawn (WorldPicture::BeginMap), and
// a world let go of on a thread of its own once it is no longer drawn (WorldPicture::SwapMap).
//
// The world being opened is made with a LoadingDevice in place of the real device: every part of it keeps that device for
// as long as the world lives, and the LoadingDevice hands each call on to the real one. While the world is being opened
// (Loading), the calls made on other threads than the frames' are not made there: what takes time of a texture or a static
// buffer is done on the calling thread (gfx.h, PrepareTexture / PrepareBuffer: any thread), and the rest of it (Finish), and
// every other call that makes something on the device, is asked of the frames' thread, which does what was asked between
// its frames, for at most so much time a frame (Service), while the calling thread waits for it. Pipelines are asked for in
// the background (gfx.h, BeginBackgroundPipelines: made on the device's own thread while the frames go on); a pipeline
// batch is nothing then. Once the world is drawn (Live), every call goes straight through: the frames' thread's own.
//
// When the world is let go of (Leaving), it is destroyed on a thread of its own: the device's Destroy calls its parts make
// are queued and done by the frames' thread between frames, a share each frame, as above. Most parts of a world do not let
// go of what they made on the device when they are destroyed (when a world went, its device went with it): the
// LoadingDevice keeps count of what its world made and has not let go of, and lets go of all of it once the world is gone
// (DestroyLeft), in the same shares, so that worlds opened one after another on one device do not fill its memory.
// [warmtarget] Targets likewise: their memory made on the calling thread (gfx.h, PrepareTarget), the rest asked.
//
// Calls that belong inside a frame (passes, draws, updates of buffers and textures, ...) are not to be made while the
// world is being opened; one made then is told on the error output and not made.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "gfx.h"

namespace render {

class LoadingDevice final : public gfx::Device {
 public:
  // On the frames' thread (the one that will call Service).
  explicit LoadingDevice(gfx::Device* real);
  ~LoadingDevice() override;

  gfx::Device* Real() const { return real_; }
  // The frames' thread, between frames (not between a BeginFrame and its EndFrame): what was asked of it, done, for at
  // most `budgetMs` (a call under way is finished; a negative budget: until nothing is waiting). While what it does
  // is followed closely by the next ask (a world opening asks again within some microseconds), it waits for that one
  // too, within the budget. The milliseconds it took.
  double Service(double budgetMs);
  // Something asked of the frames' thread and not done yet.
  bool Waiting() const;
  // (the frames' thread) The world is drawn from now on: every call goes straight through.
  void Live();
  // (the frames' thread) The world is let go of: frame generation, where it was this world's, switched off now (the
  // world's own SetFrameGeneration from then on does nothing: the next world may have it); its Destroy calls from other
  // threads queued from now.
  void Leave();
  // (the frames' thread, once the world is destroyed) What it made and never let go of: let go of by the next Services.
  void DestroyLeft();
  // (the frames' thread) Warm frames: the world opened, drawn once or twice unseen before it is shown, so that what its
  // first frame makes is made then (WorldPicture: MapReady waits for them). While on, its pipelines are asked for in the
  // background (made on the device's own thread) and draws and dispatches by one not made yet are left out (the frame is
  // not shown), so that a warm frame does not wait for them; TakeWarmth says how many such there were (pipelines asked
  // for, sets of one not made yet) since the last call: a warm frame with none of either has made all its first frame
  // wants. (With multi-core recording merged, [mcrec]: ParallelRecorders 0 while warming, so that its draws come here.)
  void SetWarming(bool on) {
    warming_ = on;
    skipping_ = false;  // (draws go through again, whatever the last warm frame set)
  }
  void TakeWarmth(int* pipelinesAsked, int* notMadeSet) {
    *pipelinesAsked = warmAsked_;
    *notMadeSet = warmMissed_;
    warmAsked_ = warmMissed_ = 0;
  }
  // [warmtarget] A warm frame recorded on the loading thread and made on the frames' thread. What a world's first frame
  // makes for the first time (its pictures at the window's size, the textures it reads first then, its pipelines, its
  // tables) took the frames' thread up to 1.4 s in one warm frame; recorded, the world's Begin and Render run on the loading
  // thread instead, so that what they make goes the way of everything a world opening makes (the slow halves on that
  // thread, the rest asked of the frames' thread a share each frame), while the calls that belong in a frame (passes,
  // pipelines and what they read, draws, dispatches, updates, the scene's build, what is let go of meanwhile) are kept here,
  // in their order, with copies of what they hand over. Replay, inside a frame the frames' thread has begun, then makes
  // them as the world would have made them there, through this device as a warm frame's (SetWarming): the frame on the card
  // is the one a warm frame drawn there would be, and the frames' thread spends only the calls' own time on it.
  // BeginRecording / EndRecording on the loading thread (the world's, which no other thread touches meanwhile); Replay on
  // the frames' thread once EndRecording has returned (the caller hands it over); a recording not replayed is let go of
  // with DropRecording (what it would have let go of stays made: DestroyLeft finds it).
  void BeginRecording();
  void EndRecording();
  void Replay();
  void DropRecording();
  // The scene's builds of the recording alone (its ray-traced structures, built the first time: 5 ms of a 4K warm frame on
  // Direct3D 12), inside a frame of their own before the one that replays it all (which builds the scene again, quickly).
  // False: the recording builds none (nothing done).
  bool ReplayScenes();
  bool RecordedScenes() const {
    for (const Call& k : rec_.calls)
      if (k.op == kBuildScene) return true;
    return false;
  }
  // What was asked of the frames' thread: how many asks, the longest Service and all of them together, milliseconds.
  struct Numbers {
    long asks = 0;
    double longestMs = 0, totalMs = 0;
    long left = 0;  // (handles let go of by DestroyLeft)
  };
  Numbers TakeNumbers() const;

  // --- gfx::Device, handed on ---
  const char* BackendName() const override { return real_->BackendName(); }
  std::string AdapterName() const override { return real_->AdapterName(); }
  int Width() const override { return real_->Width(); }
  int Height() const override { return real_->Height(); }
  void Resize(int width, int height) override;
  void SetVsync(bool on) override;
  gfx::Buffer CreateBuffer(const gfx::BufferDesc& desc) override;
  void UpdateBuffer(gfx::Buffer buffer, const void* data, uint32_t size) override;
  void* WriteBuffer(gfx::Buffer buffer, uint32_t size) override;
  gfx::Texture CreateTexture(const gfx::TextureDesc& desc) override;
  void UpdateTexture(gfx::Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) override;
  gfx::Texture CreateTarget(const gfx::TargetDesc& desc) override;
  gfx::Pipeline CreatePipeline(const gfx::PipelineDesc& desc, std::string* error) override;
  gfx::Pipeline CreateCompute(const gfx::ComputeDesc& desc, std::string* error) override;
  void BeginPipelineBatch() override;
  bool EndPipelineBatch(std::string* error) override;
  void BeginBackgroundPipelines() override;
  void EndBackgroundPipelines() override;
  std::unique_ptr<Prepared> PrepareTexture(const gfx::TextureDesc& desc) override { return real_->PrepareTexture(desc); }
  gfx::Texture FinishTexture(std::unique_ptr<Prepared> prepared) override;
  std::unique_ptr<Prepared> PrepareBuffer(const gfx::BufferDesc& desc) override { return real_->PrepareBuffer(desc); }
  gfx::Buffer FinishBuffer(std::unique_ptr<Prepared> prepared) override;
  std::unique_ptr<Prepared> PrepareTarget(const gfx::TargetDesc& desc) override { return real_->PrepareTarget(desc); }  // [warmtarget]
  gfx::Texture FinishTarget(std::unique_ptr<Prepared> prepared) override;
  void Destroy(gfx::Buffer buffer) override;
  void Destroy(gfx::Texture texture) override;
  void Destroy(gfx::Pipeline pipeline) override;
  bool RayTracing() const override { return real_->RayTracing(); }
  gfx::Structure CreateGeometry(const gfx::GeometryPart* parts, int count) override;
  std::unique_ptr<Prepared> DescribeGeometry(const gfx::GeometryPart* parts, int count) override;  // [warmtarget]
  void SizeGeometry(Prepared* prepared) override { real_->SizeGeometry(prepared); }
  gfx::Structure FinishGeometry(std::unique_ptr<Prepared> prepared) override;
  void BuildScene(gfx::Structure* scene, const gfx::SceneInstance* instances, uint32_t count) override;
  // [cardcost] (live: the real device's; recording or not now: false, so that BuildScene takes it)
  bool RefitScene(gfx::Structure* scene, const gfx::SceneInstance* instances, uint32_t count, bool rebuild) override;
  void SetScene(int slot, gfx::Structure scene) override;
  void Destroy(gfx::Structure structure) override;
  void BeginFrame(const float clearColor[4]) override;
  void BeginPass(const gfx::PassDesc& pass) override;
  void Resolve(gfx::Texture from, gfx::Texture to) override;
  void SetPipeline(gfx::Pipeline pipeline) override {
    if (Recording()) return Keep(kSetPipeline, pipeline.id);  // [warmtarget]
    if (warming_ && !real_->PipelineReady(pipeline)) {  // (a warm frame: not waited for; what it draws left out)
      skipping_ = true;
      ++warmMissed_;
      return;
    }
    skipping_ = false;
    real_->SetPipeline(pipeline);
  }
  void SetConstants(int slot, const void* data, uint32_t size) override {
    if (Recording()) return KeepBytes(kSetConstants, static_cast<uint32_t>(slot), data, size);  // [warmtarget]
    real_->SetConstants(slot, data, size);
  }
  void SetTexture(int slot, gfx::Texture texture) override {
    if (Recording()) return Keep(kSetTexture, static_cast<uint32_t>(slot), texture.id);  // [warmtarget]
    real_->SetTexture(slot, texture);
  }
  void SetStorage(int slot, gfx::Texture texture, int mip) override {
    if (Recording()) return Keep(kSetStorage, static_cast<uint32_t>(slot), texture.id, static_cast<uint32_t>(mip));  // [warmtarget]
    real_->SetStorage(slot, texture, mip);
  }
  void Dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  void SetVertexBuffer(gfx::Buffer buffer) override {
    if (Recording()) return Keep(kSetVertexBuffer, buffer.id);  // [warmtarget]
    real_->SetVertexBuffer(buffer);
  }
  void SetIndexBuffer(gfx::Buffer buffer) override {
    if (Recording()) return Keep(kSetIndexBuffer, buffer.id);  // [warmtarget]
    real_->SetIndexBuffer(buffer);
  }
  void SetShaderBuffer(gfx::Buffer buffer, int slot) override {
    if (Recording()) return Keep(kSetShaderBuffer, buffer.id, static_cast<uint32_t>(slot));  // [warmtarget]
    real_->SetShaderBuffer(buffer, slot);
  }
  void Draw(uint32_t vertexCount, uint32_t firstVertex, uint32_t instances) override {
    if (Recording()) return Keep(kDraw, vertexCount, firstVertex, instances);  // [warmtarget]
    if (!skipping_) real_->Draw(vertexCount, firstVertex, instances);
  }
  void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, uint32_t instances) override {
    if (Recording()) return Keep(kDrawIndexed, indexCount, firstIndex, instances, baseVertex);  // [warmtarget]
    if (!skipping_) real_->DrawIndexed(indexCount, firstIndex, baseVertex, instances);
  }
  void EndFrame(std::vector<uint8_t>* capture, bool present) override;
  bool SetFrameGeneration(std::function<bool()> make) override;
  bool FrameGenerating() const override { return real_->FrameGenerating(); }
  gfx::Texture WindowPicture() override { return real_->WindowPicture(); }
  gfx::Texture BetweenPicture() override { return real_->BetweenPicture(); }
  bool SetPhotoWindow(int width, int height) override;
  bool SetLatencyLimiter(bool on) override;
  bool LatencyLimiter() const override { return real_->LatencyLimiter(); }
  void WaitForNextFrame() override;
  void Mark(const char* name) override {
    if (Recording()) return KeepMark(name);  // [warmtarget]
    real_->Mark(name);
  }
  void TakeTimes(std::vector<std::pair<const char*, double>>* out) override;
  uint64_t MemoryInUse() override { return real_->MemoryInUse(); }
  bool MemoryBudget(uint64_t* budget, uint64_t* used) override { return real_->MemoryBudget(budget, used); }
  int BackgroundPipelinesWaiting() override { return real_->BackgroundPipelinesWaiting(); }
  // [mcrec] draws recorded on several threads: the device's own, once the world is drawn; none while it warms (its draws
  // then come here, where SetPipeline leaves out what is not made yet) or before (a world being opened draws nothing)
  int ParallelRecorders() const override {
    return warming_ || mode_.load(std::memory_order_acquire) != kLive ? 0 : real_->ParallelRecorders();
  }
  bool BeginParallel(int count) override { return ParallelRecorders() > 0 && real_->BeginParallel(count); }
  gfx::Recorder* Parallel(int index) override { return real_->Parallel(index); }
  void EndParallel() override { real_->EndParallel(); }

 private:
  enum Mode { kLoading, kLive, kLeaving };
  // Whether a call is made here and now: the world is drawn, or this is the frames' thread.
  bool Direct() const { return mode_.load(std::memory_order_acquire) == kLive || std::this_thread::get_id() == frames_; }
  // `work` done on the frames' thread (the next Service), this thread waiting for it.
  void Ask(const char* what, std::function<void()> work);
  // ... or not waiting (a Destroy while Leaving)
  void Queue(const char* what, std::function<void()> work);
  // A call that belongs in a frame, made by the world being opened: told once, not made.
  void NotNow(const char* what);
  // What the world has made and not let go of (all threads; under `kept_`'s lock).
  void Made(int kind, uint32_t id);
  void Gone(int kind, uint32_t id);

  gfx::Device* real_;
  std::thread::id frames_;
  std::atomic<int> mode_{kLoading};
  bool framegenOwned_ = false;  // (the world's frame generation is the device's now)
  bool warming_ = false, skipping_ = false;  // (SetWarming; a warm frame's pipeline set last not made yet)
  int warmAsked_ = 0, warmMissed_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable done_;
  struct Job {
    std::function<void()> work;
    bool* done = nullptr;  // (null: no one waits)
    const char* what = "";  // (for the hitch log: an ask that took long is noted by it)
  };
  std::deque<Job> jobs_;
  std::mutex kept_;
  std::unordered_set<uint32_t> made_[4];  // buffers, textures, pipelines, structures
  std::unordered_set<std::string> told_;
  Numbers numbers_;

  // [warmtarget] the warm frame being recorded (BeginRecording) or waiting for Replay: each call kept, with what it hands
  // over copied into `bytes` (constants, contents, rectangles), the passes and the scene's instances beside
  enum Op : uint8_t {
    kSetPipeline, kSetConstants, kSetTexture, kSetStorage, kDispatch, kSetVertexBuffer, kSetIndexBuffer, kSetShaderBuffer, kDraw,
    kDrawIndexed, kBeginPass, kResolve, kMark, kUpdateBuffer, kWriteBuffer, kUpdateTexture, kBuildScene, kSetScene, kSetSceneBuilt,
    kDestroyBuffer, kDestroyTexture, kDestroyPipeline, kDestroyStructure
  };
  struct Call {
    Op op;
    int32_t e = 0;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    size_t at = 0;      // (where its bytes, pass, instances or written room are)
    uint32_t size = 0;  // (how many bytes, or instances)
    const char* name = nullptr;
    gfx::Structure* scene = nullptr;  // (BuildScene's, and SetScene's of the scene built: made or made anew when replayed)
  };
  struct Kept {
    std::vector<Call> calls;
    std::vector<uint8_t> bytes;
    std::vector<std::unique_ptr<uint8_t[]>> written;  // (WriteBuffer's room: where the world writes, until the frame's end)
    std::vector<gfx::PassDesc> passes;
    std::vector<gfx::SceneInstance> instances;
    gfx::Structure* built = nullptr;  // (the scene built last, and its handle when it was: SetScene of it is of the one built)
    uint32_t builtWas = 0;
    int pipelinesAsked = 0;
  };
  // Whether this call is to be kept (a warm frame being recorded, and this the loading thread).
  bool Recording() const { return recording_.load(std::memory_order_relaxed) && std::this_thread::get_id() != frames_; }
  void Keep(Op op, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, int32_t e = 0) {
    Call k{op};
    k.a = a, k.b = b, k.c = c, k.e = e;
    rec_.calls.push_back(k);
  }
  void KeepBytes(Op op, uint32_t a, const void* data, size_t size, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, int32_t e = 0);
  void KeepMark(const char* name) {
    Call k{kMark};
    k.name = name;
    rec_.calls.push_back(k);
  }
  std::atomic<bool> recording_{false};
  Kept rec_;
  std::unordered_map<uint32_t, int> pixelBytes_;  // (the bytes a pixel takes of each texture made here: UpdateTexture's copy; under kept_)
};

}  // namespace render
