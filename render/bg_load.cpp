// [bgload] (bg_load.h)
#include "bg_load.h"

#include <chrono>
#include <cstdio>

#include "hitch_log.h"

namespace render {

namespace {
using Clock = std::chrono::steady_clock;
enum Kind { kBuffers, kTextures, kPipelines, kStructures };
// How long Service waits for the next ask after one it has done, within its budget (a world opening asks again within
// microseconds to a millisecond or two, between two parts of its work; longer, and it is busy with its own for a while)
constexpr double kNextAskMs = 1.5;
}  // namespace

LoadingDevice::LoadingDevice(gfx::Device* real) : real_(real), frames_(std::this_thread::get_id()) {}

LoadingDevice::~LoadingDevice() {
  Service(-1);  // (what is still waiting: done now; the threads that asked are gone)
}

void LoadingDevice::Live() { mode_.store(kLive, std::memory_order_release); }

void LoadingDevice::Leave() {
  if (framegenOwned_) real_->SetFrameGeneration({});
  framegenOwned_ = false;
  mode_.store(kLeaving, std::memory_order_release);
}

void LoadingDevice::Ask(const char* what, std::function<void()> work) {
  bool done = false;
  std::unique_lock<std::mutex> lock(mutex_);
  jobs_.push_back({std::move(work), &done, what});
  done_.wait(lock, [&] { return done; });
}

void LoadingDevice::Queue(const char* what, std::function<void()> work) {
  std::lock_guard<std::mutex> lock(mutex_);
  jobs_.push_back({std::move(work), nullptr, what});
}

bool LoadingDevice::Waiting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !jobs_.empty();
}

double LoadingDevice::Service(double budgetMs) {
  const Clock::time_point start = Clock::now();
  const bool unbounded = budgetMs < 0;
  const Clock::time_point end = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(unbounded ? 0 : budgetMs));
  long ran = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (jobs_.empty()) {
      // (once it has begun: the rest of the budget given to what comes next, as a world opening asks again as soon as it
      // has done its own work between two asks, a few microseconds to a millisecond or two; no one asking for longer than
      // kNextAskMs: nothing will for a while)
      if (ran == 0 || unbounded) break;
      const Clock::time_point next = std::min(end, Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(kNextAskMs)));
      // (looked for again and again, not waited for: a timed wait on Windows wakes on the system's timer, up to 15.6 ms late)
      while (jobs_.empty() && Clock::now() < next) {
        lock.unlock();
        std::this_thread::yield();
        lock.lock();
      }
      if (jobs_.empty()) break;
    }
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    lock.unlock();
    {
      HitchLog::Scope noted("asked by a world behind:", job.what);  // [hitches] (each ask, where the log is on)
      job.work();
    }
    lock.lock();
    ++ran;
    if (job.done) {
      *job.done = true;
      done_.notify_all();
    }
    if (!unbounded && Clock::now() >= end) break;
  }
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  if (ran > 0) {
    numbers_.asks += ran;
    numbers_.totalMs += ms;
    numbers_.longestMs = std::max(numbers_.longestMs, ms);
  }
  return ms;
}

LoadingDevice::Numbers LoadingDevice::TakeNumbers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return numbers_;
}

void LoadingDevice::NotNow(const char* what) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (told_.insert(what).second) fprintf(stderr, "render: a world being opened behind called %s (it belongs in a frame): not made\n", what);
}

void LoadingDevice::Made(int kind, uint32_t id) {
  if (!id) return;
  std::lock_guard<std::mutex> lock(kept_);
  made_[kind].insert(id);
}

void LoadingDevice::Gone(int kind, uint32_t id) {
  if (!id) return;
  std::lock_guard<std::mutex> lock(kept_);
  made_[kind].erase(id);
}

void LoadingDevice::DestroyLeft() {
  std::unordered_set<uint32_t> left[4];
  {
    std::lock_guard<std::mutex> lock(kept_);
    for (int k = 0; k < 4; ++k) left[k].swap(made_[k]);
  }
  long count = 0;
  for (int k = 0; k < 4; ++k) {
    count += static_cast<long>(left[k].size());
    for (uint32_t id : left[k]) {
      gfx::Device* real = real_;
      switch (k) {
        case kBuffers: Queue("DestroyLeft", [real, id] { real->Destroy(gfx::Buffer{id}); }); break;
        case kTextures: Queue("DestroyLeft", [real, id] { real->Destroy(gfx::Texture{id}); }); break;
        case kPipelines: Queue("DestroyLeft", [real, id] { real->Destroy(gfx::Pipeline{id}); }); break;
        default: Queue("DestroyLeft", [real, id] { real->Destroy(gfx::Structure{id}); }); break;
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  numbers_.left += count;
}

// --- what makes something: here, or asked of the frames' thread ---

gfx::Buffer LoadingDevice::CreateBuffer(const gfx::BufferDesc& desc) {
  gfx::Buffer b;
  if (Direct()) {
    b = real_->CreateBuffer(desc);
  } else if (!desc.dynamic) {  // (the slow half here, the rest there)
    std::unique_ptr<Prepared> p = real_->PrepareBuffer(desc);
    if (p) Ask("FinishBuffer", [&] { b = real_->FinishBuffer(std::move(p)); });
    else Ask("CreateBuffer", [&] { b = real_->CreateBuffer(desc); });
  } else {
    Ask("CreateBuffer (dynamic)", [&] { b = real_->CreateBuffer(desc); });
  }
  Made(kBuffers, b.id);
  return b;
}

gfx::Texture LoadingDevice::CreateTexture(const gfx::TextureDesc& desc) {
  gfx::Texture t;
  if (Direct()) {
    t = real_->CreateTexture(desc);
  } else {
    std::unique_ptr<Prepared> p = real_->PrepareTexture(desc);
    if (p) Ask("FinishTexture", [&] { t = real_->FinishTexture(std::move(p)); });
    else Ask("CreateTexture", [&] { t = real_->CreateTexture(desc); });
  }
  Made(kTextures, t.id);
  return t;
}

gfx::Texture LoadingDevice::CreateTarget(const gfx::TargetDesc& desc) {
  gfx::Texture t;
  if (Direct()) t = real_->CreateTarget(desc);
  else Ask("CreateTarget", [&] { t = real_->CreateTarget(desc); });
  Made(kTextures, t.id);
  return t;
}

gfx::Texture LoadingDevice::FinishTexture(std::unique_ptr<Prepared> prepared) {
  gfx::Texture t;
  if (Direct()) t = real_->FinishTexture(std::move(prepared));
  else Ask("FinishTexture (its own)", [&] { t = real_->FinishTexture(std::move(prepared)); });
  Made(kTextures, t.id);
  return t;
}

gfx::Buffer LoadingDevice::FinishBuffer(std::unique_ptr<Prepared> prepared) {
  gfx::Buffer b;
  if (Direct()) b = real_->FinishBuffer(std::move(prepared));
  else Ask("FinishBuffer (its own)", [&] { b = real_->FinishBuffer(std::move(prepared)); });
  Made(kBuffers, b.id);
  return b;
}

// (asked for while the world opens: made on the device's own thread, the frames going on; where the device cannot, at once)
gfx::Pipeline LoadingDevice::CreatePipeline(const gfx::PipelineDesc& desc, std::string* error) {
  gfx::Pipeline p;
  if (warming_ && std::this_thread::get_id() == frames_) {  // (a warm frame's: in the background too)
    ++warmAsked_;
    real_->BeginBackgroundPipelines();
    p = real_->CreatePipeline(desc, error);
    real_->EndBackgroundPipelines();
  } else if (Direct()) {
    p = real_->CreatePipeline(desc, error);
  } else {
    Ask(desc.name, [&] {
      real_->BeginBackgroundPipelines();
      p = real_->CreatePipeline(desc, error);
      real_->EndBackgroundPipelines();
    });
  }
  Made(kPipelines, p.id);
  return p;
}

gfx::Pipeline LoadingDevice::CreateCompute(const gfx::ComputeDesc& desc, std::string* error) {
  gfx::Pipeline p;
  if (warming_ && std::this_thread::get_id() == frames_) {
    ++warmAsked_;
    real_->BeginBackgroundPipelines();
    p = real_->CreateCompute(desc, error);
    real_->EndBackgroundPipelines();
  } else if (Direct()) {
    p = real_->CreateCompute(desc, error);
  } else {
    Ask(desc.name, [&] {
      real_->BeginBackgroundPipelines();
      p = real_->CreateCompute(desc, error);
      real_->EndBackgroundPipelines();
    });
  }
  Made(kPipelines, p.id);
  return p;
}

// (a world opening behind: its pipelines are made in the background, not in a batch)
void LoadingDevice::BeginPipelineBatch() {
  if (Direct()) real_->BeginPipelineBatch();
}
bool LoadingDevice::EndPipelineBatch(std::string* error) { return Direct() ? real_->EndPipelineBatch(error) : true; }
void LoadingDevice::BeginBackgroundPipelines() {
  if (Direct()) real_->BeginBackgroundPipelines();
}
void LoadingDevice::EndBackgroundPipelines() {
  if (Direct()) real_->EndBackgroundPipelines();
}

gfx::Structure LoadingDevice::CreateGeometry(const gfx::GeometryPart* parts, int count) {
  gfx::Structure s;
  if (Direct()) s = real_->CreateGeometry(parts, count);
  else Ask("CreateGeometry", [&] { s = real_->CreateGeometry(parts, count); });
  Made(kStructures, s.id);
  return s;
}

// --- letting go: here, or queued for the frames' thread (Destroy is deferred until the card is done anyway) ---

void LoadingDevice::Destroy(gfx::Buffer buffer) {
  Gone(kBuffers, buffer.id);
  if (Direct()) real_->Destroy(buffer);
  else Queue("Destroy", [this, buffer] { real_->Destroy(buffer); });
}
void LoadingDevice::Destroy(gfx::Texture texture) {
  Gone(kTextures, texture.id);
  if (Direct()) real_->Destroy(texture);
  else Queue("Destroy", [this, texture] { real_->Destroy(texture); });
}
void LoadingDevice::Destroy(gfx::Pipeline pipeline) {
  Gone(kPipelines, pipeline.id);
  if (Direct()) real_->Destroy(pipeline);
  else Queue("Destroy", [this, pipeline] { real_->Destroy(pipeline); });
}
void LoadingDevice::Destroy(gfx::Structure structure) {
  Gone(kStructures, structure.id);
  if (Direct()) real_->Destroy(structure);
  else Queue("Destroy", [this, structure] { real_->Destroy(structure); });
}

bool LoadingDevice::SetFrameGeneration(std::function<bool()> make) {
  if (mode_.load(std::memory_order_acquire) == kLeaving) return !make;  // (the world going: the device's is the next one's)
  if (!Direct()) {
    NotNow("SetFrameGeneration");
    return !make;
  }
  const bool on = static_cast<bool>(make);
  const bool ok = real_->SetFrameGeneration(std::move(make));
  framegenOwned_ = on && ok;
  return ok;
}

// --- what belongs in a frame: only once the world is drawn ---

void LoadingDevice::Resize(int width, int height) {
  if (Direct()) real_->Resize(width, height);
  else NotNow("Resize");
}
void LoadingDevice::SetVsync(bool on) {
  if (Direct()) real_->SetVsync(on);
  else NotNow("SetVsync");
}
void LoadingDevice::UpdateBuffer(gfx::Buffer buffer, const void* data, uint32_t size) {
  if (Direct()) real_->UpdateBuffer(buffer, data, size);
  else NotNow("UpdateBuffer");
}
void* LoadingDevice::WriteBuffer(gfx::Buffer buffer, uint32_t size) {
  if (Direct()) return real_->WriteBuffer(buffer, size);
  NotNow("WriteBuffer");
  return nullptr;
}
void LoadingDevice::UpdateTexture(gfx::Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) {
  if (Direct()) real_->UpdateTexture(texture, x, y, width, height, data, pitch);
  else NotNow("UpdateTexture");
}
void LoadingDevice::BuildScene(gfx::Structure* scene, const gfx::SceneInstance* instances, uint32_t count) {
  if (Direct()) {
    const uint32_t before = scene ? scene->id : 0;
    real_->BuildScene(scene, instances, count);
    if (scene && scene->id != before) {  // (the scene's structure, made the first time, or made anew larger)
      Gone(kStructures, before);
      Made(kStructures, scene->id);
    }
  } else {
    NotNow("BuildScene");
  }
}
void LoadingDevice::SetScene(int slot, gfx::Structure scene) {
  if (Direct()) real_->SetScene(slot, scene);
  else NotNow("SetScene");
}
void LoadingDevice::BeginFrame(const float clearColor[4]) {
  if (Direct()) real_->BeginFrame(clearColor);
  else NotNow("BeginFrame");
}
void LoadingDevice::BeginPass(const gfx::PassDesc& pass) {
  if (Direct()) real_->BeginPass(pass);
  else NotNow("BeginPass");
}
void LoadingDevice::Resolve(gfx::Texture from, gfx::Texture to) {
  if (Direct()) real_->Resolve(from, to);
  else NotNow("Resolve");
}
void LoadingDevice::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (skipping_) return;  // (a warm frame's, by a pipeline not made yet)
  if (Direct()) real_->Dispatch(x, y, z);
  else NotNow("Dispatch");
}
void LoadingDevice::EndFrame(std::vector<uint8_t>* capture, bool present) {
  if (Direct()) real_->EndFrame(capture, present);
  else NotNow("EndFrame");
}
bool LoadingDevice::SetPhotoWindow(int width, int height) {
  if (Direct()) return real_->SetPhotoWindow(width, height);
  NotNow("SetPhotoWindow");
  return width <= 0 || height <= 0;
}
bool LoadingDevice::SetLatencyLimiter(bool on) {
  if (Direct()) return real_->SetLatencyLimiter(on);
  NotNow("SetLatencyLimiter");
  return !on;
}
void LoadingDevice::WaitForNextFrame() {
  if (Direct()) real_->WaitForNextFrame();
}
void LoadingDevice::TakeTimes(std::vector<std::pair<const char*, double>>* out) {
  if (Direct()) real_->TakeTimes(out);
  else out->clear();
}

}  // namespace render
