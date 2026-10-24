// [bgload] (bg_load.h)
#include "bg_load.h"

#include <chrono>
#include <cstdio>
#include <cstring>

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
  if (kind == kTextures) pixelBytes_.erase(id);  // [warmtarget]
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
  if (t.id && !gfx::IsBlockCompressed(desc.format) && desc.mips == 1 && desc.layers == 1) {  // [warmtarget] (UpdateTexture's copy)
    std::lock_guard<std::mutex> lock(kept_);
    pixelBytes_[t.id] = gfx::BytesPerPixelOrBlock(desc.format);
  }
  return t;
}

gfx::Texture LoadingDevice::CreateTarget(const gfx::TargetDesc& desc) {
  gfx::Texture t;
  if (Direct()) {
    t = real_->CreateTarget(desc);
  } else {  // [warmtarget] (its memory made here, the rest there)
    const Clock::time_point start = Clock::now();
    std::unique_ptr<Prepared> p = real_->PrepareTarget(desc);
    const double prepared = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (p) Ask("FinishTarget", [&] { t = real_->FinishTarget(std::move(p)); });
    else Ask("CreateTarget", [&] { t = real_->CreateTarget(desc); });
    if (HitchLog::on && prepared > 2) {  // [hitches] (what took long, said: on this thread, not the frames')
      char text[160];
      snprintf(text, sizeof text, "(a target %dx%d, %d layers, %d samples%s: made behind in %.1f ms)", desc.width, desc.height, desc.layers, desc.samples,
               desc.storage ? ", storage" : "", prepared);
      HitchLog::Add(text, prepared);
    }
  }
  Made(kTextures, t.id);
  return t;
}

gfx::Texture LoadingDevice::FinishTarget(std::unique_ptr<Prepared> prepared) {  // [warmtarget]
  gfx::Texture t;
  if (Direct()) t = real_->FinishTarget(std::move(prepared));
  else Ask("FinishTarget (its own)", [&] { t = real_->FinishTarget(std::move(prepared)); });
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
    if (Recording()) ++rec_.pipelinesAsked;  // [warmtarget] (a recorded warm frame's: asked for, as a warm frame's are)
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
    if (Recording()) ++rec_.pipelinesAsked;  // [warmtarget]
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
  if (Direct()) {
    s = real_->CreateGeometry(parts, count);
  } else {  // [warmtarget] (described there, sized here - the driver's slow part -, finished there)
    std::unique_ptr<Prepared> p;
    Ask("DescribeGeometry", [&] { p = real_->DescribeGeometry(parts, count); });
    real_->SizeGeometry(p.get());
    Ask("FinishGeometry", [&] { s = real_->FinishGeometry(std::move(p)); });
  }
  Made(kStructures, s.id);
  return s;
}

std::unique_ptr<gfx::Device::Prepared> LoadingDevice::DescribeGeometry(const gfx::GeometryPart* parts, int count) {  // [warmtarget]
  std::unique_ptr<Prepared> p;
  if (Direct()) p = real_->DescribeGeometry(parts, count);
  else Ask("DescribeGeometry", [&] { p = real_->DescribeGeometry(parts, count); });
  return p;
}

gfx::Structure LoadingDevice::FinishGeometry(std::unique_ptr<Prepared> prepared) {
  gfx::Structure s;
  if (Direct()) s = real_->FinishGeometry(std::move(prepared));
  else Ask("FinishGeometry", [&] { s = real_->FinishGeometry(std::move(prepared)); });
  Made(kStructures, s.id);
  return s;
}

// --- letting go: here, or queued for the frames' thread (Destroy is deferred until the card is done anyway) ---

void LoadingDevice::Destroy(gfx::Buffer buffer) {
  if (Recording()) return Keep(kDestroyBuffer, buffer.id);  // [warmtarget] (in its place among the frame's calls)
  Gone(kBuffers, buffer.id);
  if (Direct()) real_->Destroy(buffer);
  else Queue("Destroy", [this, buffer] { real_->Destroy(buffer); });
}
void LoadingDevice::Destroy(gfx::Texture texture) {
  if (Recording()) return Keep(kDestroyTexture, texture.id);  // [warmtarget] (in its place among the frame's calls)
  Gone(kTextures, texture.id);
  if (Direct()) real_->Destroy(texture);
  else Queue("Destroy", [this, texture] { real_->Destroy(texture); });
}
void LoadingDevice::Destroy(gfx::Pipeline pipeline) {
  if (Recording()) return Keep(kDestroyPipeline, pipeline.id);  // [warmtarget] (in its place among the frame's calls)
  Gone(kPipelines, pipeline.id);
  if (Direct()) real_->Destroy(pipeline);
  else Queue("Destroy", [this, pipeline] { real_->Destroy(pipeline); });
}
void LoadingDevice::Destroy(gfx::Structure structure) {
  if (Recording()) return Keep(kDestroyStructure, structure.id);  // [warmtarget] (in its place among the frame's calls)
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
  if (Recording()) return KeepBytes(kUpdateBuffer, buffer.id, data, size);  // [warmtarget]
  if (Direct()) real_->UpdateBuffer(buffer, data, size);
  else NotNow("UpdateBuffer");
}
void* LoadingDevice::WriteBuffer(gfx::Buffer buffer, uint32_t size) {
  if (Recording()) {  // [warmtarget] (room of its own, written by the world until the frame's end; copied when replayed)
    Call k{kWriteBuffer};
    k.a = buffer.id;
    k.size = size;
    k.at = rec_.written.size();
    rec_.written.push_back(std::make_unique<uint8_t[]>(size ? size : 1));
    rec_.calls.push_back(k);
    return rec_.written.back().get();
  }
  if (Direct()) return real_->WriteBuffer(buffer, size);
  NotNow("WriteBuffer");
  return nullptr;
}
void LoadingDevice::UpdateTexture(gfx::Texture texture, int x, int y, int width, int height, const void* data, uint32_t pitch) {
  if (Recording()) {  // [warmtarget] (its rows copied tightly, as many bytes a row as the texture's pixels take)
    int unit = 0;
    {
      std::lock_guard<std::mutex> lock(kept_);
      const auto it = pixelBytes_.find(texture.id);
      if (it != pixelBytes_.end()) unit = it->second;
    }
    if (unit <= 0 || width <= 0 || height <= 0 || !data) return NotNow("UpdateTexture (of a texture made elsewhere)");
    const size_t row = static_cast<size_t>(width) * static_cast<size_t>(unit);
    Call k{kUpdateTexture};
    k.a = texture.id;
    k.b = static_cast<uint32_t>(x), k.c = static_cast<uint32_t>(y), k.d = static_cast<uint32_t>(width), k.e = height;
    k.at = rec_.bytes.size();
    k.size = static_cast<uint32_t>(row * static_cast<size_t>(height));
    rec_.bytes.resize(k.at + k.size);
    for (int r = 0; r < height; ++r)
      std::memcpy(rec_.bytes.data() + k.at + static_cast<size_t>(r) * row, static_cast<const uint8_t*>(data) + static_cast<size_t>(r) * pitch, row);
    rec_.calls.push_back(k);
    return;
  }
  if (Direct()) real_->UpdateTexture(texture, x, y, width, height, data, pitch);
  else NotNow("UpdateTexture");
}
void LoadingDevice::BuildScene(gfx::Structure* scene, const gfx::SceneInstance* instances, uint32_t count) {
  if (Recording()) {  // [warmtarget] (the instances copied; the scene's handle written where the world keeps it when replayed)
    Call k{kBuildScene};
    k.scene = scene;
    k.at = rec_.instances.size();
    k.size = count;
    rec_.instances.insert(rec_.instances.end(), instances, instances + count);
    rec_.calls.push_back(k);
    rec_.built = scene;
    rec_.builtWas = scene ? scene->id : 0;
    return;
  }
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
  if (Recording()) {  // [warmtarget] (the scene built in this frame: its handle as it is when replayed)
    Call k{rec_.built && scene.id == rec_.builtWas ? kSetSceneBuilt : kSetScene};
    k.a = static_cast<uint32_t>(slot);
    k.b = scene.id;
    k.scene = rec_.built;
    rec_.calls.push_back(k);
    return;
  }
  if (Direct()) real_->SetScene(slot, scene);
  else NotNow("SetScene");
}
void LoadingDevice::BeginFrame(const float clearColor[4]) {
  if (Direct()) real_->BeginFrame(clearColor);
  else NotNow("BeginFrame");
}
void LoadingDevice::BeginPass(const gfx::PassDesc& pass) {
  if (Recording()) {  // [warmtarget]
    Call k{kBeginPass};
    k.at = rec_.passes.size();
    rec_.passes.push_back(pass);
    rec_.calls.push_back(k);
    return;
  }
  if (Direct()) real_->BeginPass(pass);
  else NotNow("BeginPass");
}
void LoadingDevice::Resolve(gfx::Texture from, gfx::Texture to) {
  if (Recording()) return Keep(kResolve, from.id, to.id);  // [warmtarget]
  if (Direct()) real_->Resolve(from, to);
  else NotNow("Resolve");
}
void LoadingDevice::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
  if (Recording()) return Keep(kDispatch, x, y, z);  // [warmtarget]
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

// --- [warmtarget] a warm frame recorded on the loading thread, made on the frames' thread ---

void LoadingDevice::KeepBytes(Op op, uint32_t a, const void* data, size_t size, uint32_t b, uint32_t c, uint32_t d, int32_t e) {
  Call k{op};
  k.a = a, k.b = b, k.c = c, k.d = d, k.e = e;
  k.at = rec_.bytes.size();
  k.size = static_cast<uint32_t>(size);
  if (size && data) rec_.bytes.insert(rec_.bytes.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  else k.size = 0;
  rec_.calls.push_back(k);
}

void LoadingDevice::BeginRecording() {
  rec_ = Kept{};
  recording_.store(true, std::memory_order_relaxed);
}

void LoadingDevice::EndRecording() { recording_.store(false, std::memory_order_relaxed); }

void LoadingDevice::DropRecording() { rec_ = Kept{}; }

bool LoadingDevice::ReplayScenes() {
  bool any = false;
  for (const Call& k : rec_.calls)
    if (k.op == kBuildScene) {
      BuildScene(k.scene, rec_.instances.data() + k.at, k.size);
      any = true;
    }
  return any;
}

void LoadingDevice::Replay() {
  Kept r = std::move(rec_);
  rec_ = Kept{};
  warmAsked_ += r.pipelinesAsked;
  HitchLog::Scope noted("a warm frame recorded behind, made");  // [hitches]
  for (const Call& k : r.calls) {
    switch (k.op) {
      case kSetPipeline: SetPipeline({k.a}); break;
      case kSetConstants: SetConstants(static_cast<int>(k.a), k.size ? r.bytes.data() + k.at : nullptr, k.size); break;
      case kSetTexture: SetTexture(static_cast<int>(k.a), {k.b}); break;
      case kSetStorage: SetStorage(static_cast<int>(k.a), {k.b}, static_cast<int>(k.c)); break;
      case kDispatch: Dispatch(k.a, k.b, k.c); break;
      case kSetVertexBuffer: SetVertexBuffer({k.a}); break;
      case kSetIndexBuffer: SetIndexBuffer({k.a}); break;
      case kSetShaderBuffer: SetShaderBuffer({k.a}, static_cast<int>(k.b)); break;
      case kDraw: Draw(k.a, k.b, k.c); break;
      case kDrawIndexed: DrawIndexed(k.a, k.b, k.e, k.c); break;
      case kBeginPass: BeginPass(r.passes[k.at]); break;
      case kResolve: Resolve({k.a}, {k.b}); break;
      case kMark: Mark(k.name); break;
      case kUpdateBuffer: UpdateBuffer({k.a}, k.size ? r.bytes.data() + k.at : nullptr, k.size); break;
      case kWriteBuffer:
        if (void* room = WriteBuffer({k.a}, k.size)) std::memcpy(room, r.written[k.at].get(), k.size);
        break;
      case kUpdateTexture:
        UpdateTexture({k.a}, static_cast<int>(k.b), static_cast<int>(k.c), static_cast<int>(k.d), k.e, r.bytes.data() + k.at,
                      k.d && k.e ? k.size / static_cast<uint32_t>(k.e) : 0);
        break;
      case kBuildScene: BuildScene(k.scene, r.instances.data() + k.at, k.size); break;
      case kSetScene: SetScene(static_cast<int>(k.a), {k.b}); break;
      case kSetSceneBuilt: SetScene(static_cast<int>(k.a), k.scene ? *k.scene : gfx::Structure{}); break;
      case kDestroyBuffer: Destroy(gfx::Buffer{k.a}); break;
      case kDestroyTexture: Destroy(gfx::Texture{k.a}); break;
      case kDestroyPipeline: Destroy(gfx::Pipeline{k.a}); break;
      case kDestroyStructure: Destroy(gfx::Structure{k.a}); break;
    }
  }
}

}  // namespace render
