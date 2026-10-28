#include "textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/vfs.h"
#include "dds.h"
#include "hitch_log.h"  // [hitches]
#include "jobs.h"
#include "linalg.h"

namespace render {
namespace {

gfx::Texture Solid(gfx::Device* device, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const uint8_t pixel[4] = {r, g, b, a};
  gfx::TextureDesc d;
  d.width = d.height = 1;
  d.format = gfx::Format::RGBA8;
  d.data = pixel;
  d.dataSize = 4;
  return device->CreateTexture(d);
}

// Leaves out the `drop` largest sizes of a one-layer texture larger than 256 whose file has the
// smaller ones (the texture detail setting): the image then starts at the next size down.
void DropLargest(DdsImage* image, int drop) {
  if (drop <= 0 || image->layers != 1 || image->cube) return;
  const bool blocks = gfx::IsBlockCompressed(image->format);
  const size_t unit = static_cast<size_t>(gfx::BytesPerPixelOrBlock(image->format));
  if (!unit) return;
  size_t skip = 0;
  int k = 0;
  while (k < drop && image->mips - k > 1 && std::max(image->width >> k, image->height >> k) > 256) {
    const size_t w = static_cast<size_t>(std::max(1, image->width >> k)), h = static_cast<size_t>(std::max(1, image->height >> k));
    skip += (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
    ++k;
  }
  if (!k || skip >= image->dataSize) return;
  image->data += skip;
  image->dataSize -= skip;
  image->width = std::max(1, image->width >> k);
  image->height = std::max(1, image->height >> k);
  image->mips -= k;
}

// [seasons] The bytes of a texture's largest levels, for as many levels as it takes to come down to `width` (0: it
// cannot: not that much larger by whole levels, or its file lacks them); `levels` how many that is.
size_t BytesAbove(const DdsImage& image, int width, int* levels) {
  *levels = 0;
  const size_t unit = static_cast<size_t>(gfx::BytesPerPixelOrBlock(image.format));
  if (!unit || image.layers != 1 || image.cube) return 0;
  const bool blocks = gfx::IsBlockCompressed(image.format);
  size_t skip = 0;
  int k = 0;
  while ((image.width >> k) > width && image.mips - k > 1) {
    const size_t w = static_cast<size_t>(std::max(1, image.width >> k)), h = static_cast<size_t>(std::max(1, image.height >> k));
    skip += (blocks ? (w + 3) / 4 * ((h + 3) / 4) : w * h) * unit;
    ++k;
  }
  if ((image.width >> k) != width || skip >= image.dataSize) return 0;
  *levels = k;
  return skip;
}

}  // namespace

TextureCache::TextureCache(gfx::Device* device, const moho::Vfs* vfs) : device_(device), vfs_(vfs) {
  white_ = Solid(device, 255, 255, 255, 255);
  black_ = Solid(device, 0, 0, 0, 0);
  flatNormal_ = Solid(device, 128, 128, 255, 128);
  uint8_t faces[6 * 4];
  for (int i = 0; i < 6; ++i) {
    faces[i * 4] = 110;
    faces[i * 4 + 1] = 130;
    faces[i * 4 + 2] = 150;
    faces[i * 4 + 3] = 255;
  }
  gfx::TextureDesc cube;
  cube.width = cube.height = 1;
  cube.layers = 6;
  cube.cube = true;
  cube.format = gfx::Format::RGBA8;
  cube.data = faces;
  cube.dataSize = sizeof faces;
  greyCube_ = device->CreateTexture(cube);
}

// [hitches] FetchLater's files: a queue read one after another by a thread of its own; what was read waits in `ready`
// until ReadFile takes it, what cannot be read is kept in `failed`.
struct TextureCache::Later {
  std::mutex mutex;
  std::condition_variable wake, done;
  std::deque<std::string> queue;
  std::string reading;  // (the file being read now)
  std::unordered_map<std::string, std::string> ready;
  std::unordered_set<std::string> failed;
  std::unordered_set<std::string> taken;  // (read and taken by a ReadFile since, or read by another instead)
  bool stop = false;
  std::thread thread;
  bool Knows(const std::string& key) const {
    return ready.count(key) || failed.count(key) || reading == key || std::find(queue.begin(), queue.end(), key) != queue.end();
  }
  void Run(const moho::Vfs* vfs) {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      wake.wait(lock, [&] { return stop || !queue.empty(); });
      if (stop) return;
      reading = std::move(queue.front());
      queue.pop_front();
      const std::string key = reading;
      lock.unlock();
      // (the engine's file layer opens a file of its own for each read: reads may run beside the main thread's, as Fetch's)
      std::optional<std::string> bytes = vfs->Exists(key) ? vfs->ReadFile(key) : std::nullopt;
      lock.lock();
      if (bytes) ready.emplace(key, std::move(*bytes));
      else failed.insert(key);
      reading.clear();
      done.notify_all();
    }
  }
  // The bytes of a file asked for (waiting while it is being read); none where it was never asked for, cannot be read,
  // or is still queued (taken out of the queue: the caller reads it itself).
  std::optional<std::string> Take(const std::string& key) {
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [&] { return reading != key; });
    auto it = ready.find(key);
    if (it != ready.end()) {
      std::optional<std::string> out(std::move(it->second));
      ready.erase(it);
      taken.insert(key);
      return out;
    }
    auto q = std::find(queue.begin(), queue.end(), key);
    if (q != queue.end()) {
      queue.erase(q);
      taken.insert(key);  // (read by the caller instead)
    }
    return std::nullopt;
  }
};

// [spikes] MakeAhead's jobs: asked for on the frames' thread, worked on by two threads of their own (the file, the image, the
// device's PrepareTexture), finished on the frames' thread (MakeAheadStep, or a Get that wants one).
struct TextureCache::Making {
  struct Job {
    std::string key;
    int detail = 0;
    std::optional<std::string> bytes;  // (what Fetch had read already, handed over when asked)
    Later* fetching = nullptr;         // (FetchLater has it or is reading it: taken from there)
    bool begun = false, done = false;
    bool missing = false;              // (no such file)
    std::string error;                 // (its image could not be read, or the device refused it)
    std::unique_ptr<gfx::Device::Prepared> prepared;
    size_t dataSize = 0;
  };
  mutable std::mutex mutex;
  std::condition_variable wake, done;
  std::deque<Job*> queue, later;                              // (not begun, in the order asked; `later` once `queue` is done)
  std::unordered_map<std::string, std::unique_ptr<Job>> jobs;  // (asked for, not finished)
  std::deque<std::string> order;                              // (the keys in the order asked: finished in it)
  bool stop = false;
  std::vector<std::thread> threads;
  void Run(const moho::Vfs* vfs, gfx::Device* device) {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      wake.wait(lock, [&] { return stop || !queue.empty() || !later.empty(); });
      if (stop) return;
      std::deque<Job*>& from = queue.empty() ? later : queue;
      Job* job = from.front();
      from.pop_front();
      job->begun = true;
      lock.unlock();
      std::optional<std::string> file = std::move(job->bytes);
      if (!file && job->fetching) file = job->fetching->Take(job->key);
      if (!file) file = vfs->Exists(job->key) ? vfs->ReadFile(job->key) : std::nullopt;
      if (!file) {
        job->missing = true;
      } else {
        DdsImage image;
        if (!ParseDds(*file, &image, &job->error)) {
          if (job->error.empty()) job->error = "not a picture";
        } else {
          DropLargest(&image, job->detail);
          job->dataSize = image.dataSize;
          job->prepared = device->PrepareTexture(image.Desc());
          if (!job->prepared) job->error = "the device refused it";
        }
      }
      file.reset();
      lock.lock();
      job->done = true;
      done.notify_all();
    }
  }
};

void TextureCache::MakeAhead(const std::vector<std::string>& paths, bool later) {
  if (!making_) {
    making_ = std::make_unique<Making>();
    Making* m = making_.get();
    const moho::Vfs* vfs = vfs_;
    gfx::Device* device = device_;
    for (int i = 0; i < 2; ++i) m->threads.emplace_back([m, vfs, device] { m->Run(vfs, device); });
  }
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(making_->mutex);
    for (const std::string& path : paths) {
      std::string key = moho::Vfs::Normalize(path);
      if (key.empty() || cache_.count(key) || making_->jobs.count(key)) continue;
      auto job = std::make_unique<Making::Job>();
      job->key = key;
      job->detail = detail;
      auto fetched = fetched_.find(key);
      if (fetched != fetched_.end()) {  // (read already by Fetch: handed over)
        job->bytes = std::move(fetched->second);
        fetched_.erase(fetched);
      } else if (later_) {
        std::lock_guard<std::mutex> lockLater(later_->mutex);
        if (later_->Knows(key)) job->fetching = later_.get();
      }
      (later ? making_->later : making_->queue).push_back(job.get());
      making_->order.push_back(key);
      making_->jobs.emplace(std::move(key), std::move(job));
      any = true;
    }
  }
  if (any) making_->wake.notify_all();
}

// (a job done: its texture made from what the threads prepared, into the cache; messages as Get's own; the job let go of.
// The caller holds no lock; the job must be done.)
gfx::Texture TextureCache::FinishAhead(const std::string& key) {
  std::unique_ptr<Making::Job> job;
  {
    std::lock_guard<std::mutex> lock(making_->mutex);
    auto it = making_->jobs.find(key);
    if (it == making_->jobs.end() || !it->second->done) return {};
    job = std::move(it->second);
    making_->jobs.erase(it);
  }
  gfx::Texture t;
  if (job->missing) {
    fprintf(stderr, "render: texture %s: file not found\n", key.c_str());
  } else if (!job->error.empty() && !job->prepared) {
    fprintf(stderr, "render: texture %s: %s\n", key.c_str(), job->error.c_str());
  } else {
    const auto start = std::chrono::steady_clock::now();
    t = device_->FinishTexture(std::move(job->prepared));
    if (HitchLog::on) {  // (one that took long: named, its device's parts under it)
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      if (ms > 1) HitchLog::Add("texture finished " + key, ms, 1);
    }
    if (!t) fprintf(stderr, "render: texture %s: the device refused it\n", key.c_str());
    else bytes += job->dataSize, madeAheadBytes += job->dataSize;
  }
  ++madeAhead;
  cache_.emplace(key, t);
  return t;
}

int TextureCache::MakeAheadStep(double budgetMs) {
  if (!making_) return 0;
  const auto start = std::chrono::steady_clock::now();
  int finished = 0;
  for (;;) {
    std::string key;
    {
      std::lock_guard<std::mutex> lock(making_->mutex);
      // (the first done in the order asked; one taken by a Get meanwhile is gone from the jobs)
      while (!making_->order.empty() && !making_->jobs.count(making_->order.front())) making_->order.pop_front();
      for (auto it = making_->order.begin(); it != making_->order.end(); ++it) {
        auto job = making_->jobs.find(*it);
        if (job == making_->jobs.end() || !job->second->done) continue;
        key = *it;
        making_->order.erase(it);
        break;
      }
      if (key.empty()) break;
    }
    FinishAhead(key);
    ++finished;
    if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budgetMs) break;
  }
  if (finished && HitchLog::on)
    HitchLog::Add("textures made ahead: " + std::to_string(finished) + " finished, " + std::to_string(AheadLeft()) + " to come",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return finished;
}

bool TextureCache::Ahead(const std::string& path) const {
  if (!making_) return false;
  std::lock_guard<std::mutex> lock(making_->mutex);
  return making_->jobs.count(moho::Vfs::Normalize(path)) > 0;
}

size_t TextureCache::AheadLeft() const {
  if (!making_) return 0;
  std::lock_guard<std::mutex> lock(making_->mutex);
  return making_->jobs.size();
}

TextureCache::~TextureCache() {
  if (making_) {  // [spikes] (its threads stopped first: a job may be taking a file from FetchLater's)
    {
      std::lock_guard<std::mutex> lock(making_->mutex);
      making_->stop = true;
    }
    making_->wake.notify_all();
    for (std::thread& t : making_->threads)
      if (t.joinable()) t.join();
    making_.reset();  // (what was prepared and not finished let go of, while the device is there)
  }
  if (!later_) return;
  {
    std::lock_guard<std::mutex> lock(later_->mutex);
    later_->stop = true;
  }
  later_->wake.notify_all();
  if (later_->thread.joinable()) later_->thread.join();
}

void TextureCache::FetchLater(const std::vector<std::string>& paths) {
  if (!later_) {
    later_ = std::make_unique<Later>();
    Later* later = later_.get();
    const moho::Vfs* vfs = vfs_;
    later_->thread = std::thread([later, vfs] { later->Run(vfs); });
  }
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(later_->mutex);
    for (const std::string& path : paths) {
      std::string key = moho::Vfs::Normalize(path);
      if (key.empty() || cache_.count(key) || fetched_.count(key) || later_->Knows(key) || (making_ && Ahead(key))) continue;  // ([spikes] Ahead)
      later_->taken.erase(key);
      later_->queue.push_back(std::move(key));
      any = true;
    }
  }
  if (any) later_->wake.notify_one();
}

int TextureCache::Arrived(const std::string& path) {
  const std::string key = moho::Vfs::Normalize(path);
  if (cache_.count(key) || fetched_.count(key)) return 1;
  if (!later_) return 0;
  std::lock_guard<std::mutex> lock(later_->mutex);
  return later_->ready.count(key) ? 1 : later_->failed.count(key) ? -1 : later_->taken.count(key) ? 2 : 0;
}

bool TextureCache::Made(const std::string& path) const { return cache_.count(moho::Vfs::Normalize(path)) > 0; }

gfx::Texture TextureCache::FromDds(std::string_view file, const char* what) { return Make(file, what, 0); }

gfx::Texture TextureCache::Make(std::string_view file, const char* what, int drop) {
  DdsImage image;
  std::string error;
  if (!ParseDds(file, &image, &error)) {
    fprintf(stderr, "render: texture %s: %s\n", what, error.c_str());
    return {};
  }
  DropLargest(&image, drop);
  gfx::Texture t = device_->CreateTexture(image.Desc());
  if (!t) fprintf(stderr, "render: texture %s: the device refused it\n", what);
  else bytes += image.dataSize;
  return t;
}

gfx::Texture TextureCache::Layers(const std::vector<std::string>& paths, std::vector<bool>* present) {
  HitchLog::Scope made("texture set", paths.empty() ? std::string() : paths[0] + " and " + std::to_string(paths.size() - 1) + " more");  // [hitches]
  present->assign(paths.size(), false);
  DdsImage first;
  std::vector<uint8_t> all;
  size_t each = 0;
  bool mixed = false;  // [seasons] (sizes that differed, read at the smallest)
  // [snowhitch] the files read side by side, a group at a time on the cores' jobs (the engine's file layer opens a file of its
  // own for each read), each group laid in before the next is read: the same set, in a third of the time (the ground's set of
  // 23 4k pictures read the first frame of a cold game: 1.0-1.6 s serially)
  const size_t group = static_cast<size_t>(Jobs::Shared().Workers()) + 1;
  std::vector<std::optional<std::string>> files;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i % group == 0) {
      const size_t n = std::min(group, paths.size() - i);
      files.assign(n, std::nullopt);
      Jobs::Shared().Run(static_cast<int>(n), [&](int k) {
        const std::string& path = paths[i + static_cast<size_t>(k)];
        if (!path.empty()) files[static_cast<size_t>(k)] = vfs_->ReadFile(path);
      });
    }
    if (paths[i].empty()) continue;
    std::optional<std::string>& file = files[i % group];
    DdsImage image;
    std::string error;
    if (!file || !ParseDds(*file, &image, &error) || image.cube) continue;
    DropLargest(&image, detail);
    // [seasons] a set whose sizes differ (a season's material at its 4k tier among 8k ones): read at the smallest's
    // size, the larger ones from that size down where their files hold it (those already laid in cut down as well)
    if (each && image.format == first.format && image.width != first.width && image.width * first.height == image.height * first.width) {
      int levels = 0;
      if (image.width > first.width) {
        const size_t skip = BytesAbove(image, first.width, &levels);
        if (skip) {
          image.data += skip;
          image.dataSize -= skip;
          image.width = first.width;
          image.height = first.height;
          image.mips -= levels;
        }
      } else if (const size_t skip = BytesAbove(first, image.width, &levels); skip && first.dataSize - skip == image.dataSize) {
        std::vector<uint8_t> cut(image.dataSize * paths.size(), 0);
        for (size_t j = 0; j < i; ++j)
          if ((*present)[j]) std::memcpy(cut.data() + image.dataSize * j, all.data() + each * j + skip, image.dataSize);
        all.swap(cut);
        each = image.dataSize;
        first.width = image.width;
        first.height = image.height;
        first.mips -= levels;
        first.dataSize = each;
      }
      mixed = mixed || image.width == first.width;
    }
    // [snowthin] a one-channel picture (BC4) in a set of two-channel ones (BC5): its channel in both (a BC5 block is two BC4
    // blocks, red then green; the levels' blocks in the same order)
    std::vector<uint8_t> widened;
    if (each && first.format == gfx::Format::BC5 && image.format == gfx::Format::BC4 && image.dataSize * 2 == each) {
      widened.resize(each);
      for (size_t b = 0; b + 8 <= image.dataSize; b += 8) {
        std::memcpy(widened.data() + b * 2, image.data + b, 8);
        std::memcpy(widened.data() + b * 2 + 8, image.data + b, 8);
      }
      image.data = widened.data();
      image.dataSize = each;
      image.format = gfx::Format::BC5;
    }
    if (!each) {
      first = image;
      each = image.dataSize;
      all.assign(each * paths.size(), 0);
    } else if (image.width != first.width || image.height != first.height || image.format != first.format || image.mips != first.mips ||
               image.dataSize != each) {
      fprintf(stderr, "render: %s: not of the size and kind of the others of its set; left out\n", paths[i].c_str());
      continue;
    }
    std::memcpy(all.data() + each * i, image.data, each);
    (*present)[i] = true;
  }
  if (!each) return {};
  if (mixed) printf("render: a set of %zu layers of different sizes read at %d pixels, its smallest\n", paths.size(), first.width);
  gfx::TextureDesc td;
  td.width = first.width;
  td.height = first.height;
  td.mips = first.mips;
  td.layers = static_cast<int>(paths.size());
  td.format = first.format;
  td.data = all.data();
  td.dataSize = all.size();
  bytes += all.size();
  return device_->CreateTexture(td);
}

namespace {

// The direction a pixel of a cube map's face looks in (the faces in the order +x, -x, +y, -y, +z,
// -z; u to the right and v down across the face, each -1..1), not of unit length.
Vec3 CubeDirection(int face, float u, float v) {
  switch (face) {
    case 0: return {1, -v, -u};
    case 1: return {-1, -v, u};
    case 2: return {u, 1, v};
    case 3: return {u, -1, -v};
    case 4: return {u, -v, 1};
    default: return {-u, -v, -1};
  }
}

// The sky cube's small levels (faces of 32 pixels and less) made anew, each pixel the light of the
// whole sky as a surface of that level's roughness gathers it about that direction, in place of
// the 2 x 2 averages (which know nothing of the neighbouring faces: the blurriest level was six
// flat colours with hard edges between them). The lighting reads level roughness x 8 for what a
// surface mirrors and level 8 for the light from all sides, so level L is gathered with the lobe
// of roughness L / 8: weight cos^e about the direction, e = (2 / roughness^4 - 2) / 4 and never
// under 1 (the plain cosine of light from all sides). Gathered from the 16 pixel level in linear
// light, each of its pixels by the piece of sky it covers.
void PrefilterSky(std::vector<std::vector<uint8_t>> levels[6], int width) {
  int from = -1;
  for (size_t l = 0; l < levels[0].size(); ++l)
    if ((width >> l) == 16) from = static_cast<int>(l);
  if (from < 1) return;  // (a cube smaller than that: left as it is)
  struct Sample {
    Vec3 direction;
    float light[3];  // linear
    float covers;    // the piece of sky the pixel covers
  };
  std::vector<Sample> sky;
  sky.reserve(6 * 16 * 16);
  for (int f = 0; f < 6; ++f)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / 8 - 1, v = (static_cast<float>(y) + 0.5f) / 8 - 1;
        const uint8_t* p = &levels[f][static_cast<size_t>(from)][(static_cast<size_t>(y) * 16 + static_cast<size_t>(x)) * 4];
        Sample s;
        s.direction = Normalize(CubeDirection(f, u, v));
        for (int c = 0; c < 3; ++c) s.light[c] = std::pow(static_cast<float>(p[c]) / 255.0f, 2.2f);
        s.covers = 1 / std::pow(1 + u * u + v * v, 1.5f);
        sky.push_back(s);
      }
  for (size_t l = 0; l < levels[0].size(); ++l) {
    const int size = width >> l;
    if (size > 32 || size < 1) continue;
    const float roughness = std::min(static_cast<float>(l) / 8.0f, 1.0f), a2 = roughness * roughness * roughness * roughness;
    const float e = std::max((2 / std::max(a2, 1e-4f) - 2) * 0.25f, 1.0f);
    for (int f = 0; f < 6; ++f)
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
          const float half = static_cast<float>(size) * 0.5f;
          const Vec3 d = Normalize(CubeDirection(f, (static_cast<float>(x) + 0.5f) / half - 1, (static_cast<float>(y) + 0.5f) / half - 1));
          float sum[3] = {0, 0, 0}, weights = 0;
          for (const Sample& s : sky) {
            const float facing = Dot(d, s.direction);
            if (facing <= 0) continue;
            const float w = (e == 1.0f ? facing : std::pow(facing, e)) * s.covers;
            sum[0] += s.light[0] * w;
            sum[1] += s.light[1] * w;
            sum[2] += s.light[2] * w;
            weights += w;
          }
          uint8_t* q = &levels[f][l][(static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4];
          for (int c = 0; c < 3; ++c)
            q[c] = static_cast<uint8_t>(std::min(std::pow(weights > 0 ? sum[c] / weights : 0.0f, 1 / 2.2f) * 255.0f + 0.5f, 255.0f));
        }
  }
}

}  // namespace

gfx::Texture TextureCache::SkyCube(const std::string& path) {
  if (path.empty()) return greyCube_;
  const std::string key = "sky:" + moho::Vfs::Normalize(path);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  gfx::Texture result = greyCube_;
  DdsImage image;
  std::string error;
  auto file = vfs_->ReadFile(moho::Vfs::Normalize(path));
  if (!file || !ParseDds(*file, &image, &error) || !image.cube || image.width != image.height) {
    fprintf(stderr, "render: sky cube %s: %s\n", path.c_str(), !file ? "file not found" : error.empty() ? "not a cube map" : error.c_str());
  } else {
    // every face as RGBA, then each level from the one above by averaging 2 x 2 pixels
    std::vector<uint8_t> all, face, next;
    std::vector<std::vector<uint8_t>> levels[6];  // per face, per level
    int mips = 0;
    bool ok = true;
    for (int layer = 0; layer < 6 && ok; ++layer) {
      ok = DecodeRgba(image, layer, &face);
      mips = 0;
      for (int size = image.width; ok && size >= 1; size /= 2, ++mips) {
        levels[layer].push_back(face);
        if (size == 1) {
          ++mips;
          break;
        }
        const int half = size / 2;
        next.resize(static_cast<size_t>(half) * static_cast<size_t>(half) * 4);
        for (int y = 0; y < half; ++y)
          for (int x = 0; x < half; ++x)
            for (int c = 0; c < 4; ++c) {
              const size_t a = (static_cast<size_t>(2 * y) * static_cast<size_t>(size) + static_cast<size_t>(2 * x)) * 4 + static_cast<size_t>(c);
              const size_t row = static_cast<size_t>(size) * 4;
              next[(static_cast<size_t>(y) * static_cast<size_t>(half) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] =
                  static_cast<uint8_t>((face[a] + face[a + 4] + face[a + row] + face[a + row + 4] + 2) / 4);
            }
        face.swap(next);
      }
    }
    if (ok) {
      const auto t0 = std::chrono::steady_clock::now();
      PrefilterSky(levels, image.width);
      printf("render: sky cube %s: its blurred levels gathered in %.0f ms\n", path.c_str(),
             std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    for (int layer = 0; layer < 6 && ok; ++layer)
      for (const std::vector<uint8_t>& level : levels[layer]) all.insert(all.end(), level.begin(), level.end());
    if (ok) {
      gfx::TextureDesc d;
      d.width = d.height = image.width;
      d.mips = mips;
      d.layers = 6;
      d.cube = true;
      d.format = gfx::Format::RGBA8;
      d.data = all.data();
      d.dataSize = all.size();
      gfx::Texture t = device_->CreateTexture(d);
      if (t) result = t;
    } else {
      fprintf(stderr, "render: sky cube %s: its pixel format cannot be decoded\n", path.c_str());
    }
  }
  cache_.emplace(key, result);
  return result;
}

void TextureCache::Fetch(const std::vector<std::string>& paths) {
  std::vector<std::string> wanted;
  for (const std::string& path : paths) {
    std::string key = moho::Vfs::Normalize(path);
    if (key.empty() || cache_.count(key) || fetched_.count(key) || std::find(wanted.begin(), wanted.end(), key) != wanted.end()) continue;
    if (making_ && Ahead(key)) continue;  // [spikes] (being made ahead: its own threads read it)
    if (later_) {  // [hitches] (one FetchLater has read or is reading: ReadFile takes it; one still queued is read here)
      std::lock_guard<std::mutex> lock(later_->mutex);
      if (later_->ready.count(key) || later_->reading == key) continue;
      auto q = std::find(later_->queue.begin(), later_->queue.end(), key);
      if (q != later_->queue.end()) {
        later_->queue.erase(q);
        later_->taken.insert(key);  // (read here instead)
      }
    }
    wanted.push_back(std::move(key));
  }
  if (wanted.empty()) return;
  HitchLog::Scope made("files read ahead:", std::to_string(wanted.size()));  // [hitches]
  // (the engine's file layer opens a file of its own for each read: reads may run side by side)
  std::vector<std::optional<std::string>> read(wanted.size());
  Jobs::Shared().Run(static_cast<int>(wanted.size()), [&](int i) {
    const std::string& key = wanted[static_cast<size_t>(i)];
    if (vfs_->Exists(key)) read[static_cast<size_t>(i)] = vfs_->ReadFile(key);
  });
  for (size_t i = 0; i < wanted.size(); ++i)
    if (read[i]) fetched_.emplace(std::move(wanted[i]), std::move(*read[i]));
}

std::optional<std::string> TextureCache::ReadFile(const std::string& path) {
  const std::string key = moho::Vfs::Normalize(path);
  auto it = fetched_.find(key);
  if (it == fetched_.end()) {
    if (later_)  // [hitches] (read ahead without waiting: FetchLater)
      if (std::optional<std::string> got = later_->Take(key)) return got;
    return vfs_->ReadFile(key);
  }
  std::optional<std::string> out(std::move(it->second));
  fetched_.erase(it);
  return out;
}

gfx::Texture TextureCache::Get(const std::string& path, gfx::Texture fallback) {
  if (path.empty()) return fallback;
  std::string key = moho::Vfs::Normalize(path);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second ? it->second : fallback;
  if (making_) {  // [spikes] asked for ahead (MakeAhead): waited for and finished here; one not begun is made here as below
    std::unique_lock<std::mutex> lock(making_->mutex);
    auto job = making_->jobs.find(key);
    if (job != making_->jobs.end() && !job->second->begun) {
      Making::Job* j = job->second.get();
      for (std::deque<Making::Job*>* q : {&making_->queue, &making_->later}) {
        auto at = std::find(q->begin(), q->end(), j);
        if (at != q->end()) q->erase(at);
      }
      std::optional<std::string> given = std::move(j->bytes);
      making_->jobs.erase(job);
      lock.unlock();
      if (given) fetched_.emplace(key, std::move(*given));  // (what it was handed: ReadFile below takes it)
    } else if (job != making_->jobs.end()) {
      HitchLog::Scope waited("texture made ahead, waited for:", key);
      making_->done.wait(lock, [&] { return job->second->done; });
      lock.unlock();
      const gfx::Texture t = FinishAhead(key);
      return t ? t : fallback;
    }
  }
  gfx::Texture t;
  HitchLog::Scope made("texture", key);  // [hitches]
  const size_t before = bytes;
  const auto start = std::chrono::steady_clock::now();
  if (auto file = ReadFile(key)) {
    const auto read = std::chrono::steady_clock::now();
    t = Make(*file, key.c_str(), detail);
    if (HitchLog::on) {  // [hitches] (its size, and how much of its time was reading the file)
      char size[64];
      snprintf(size, sizeof size, " (%.1f MB, read in %.1f ms)", static_cast<double>(bytes - before) / 1048576.0,
               std::chrono::duration<double, std::milli>(read - start).count());
      made.Append(size);
    }
  } else {
    fprintf(stderr, "render: texture %s: file not found\n", key.c_str());
  }
  cache_.emplace(std::move(key), t);
  return t ? t : fallback;
}

}  // namespace render
