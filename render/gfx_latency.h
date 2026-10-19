// The latency limiter's part the backends share (gfx.h, Device::SetLatencyLimiter): when a frame should begin, so
// that its input is read as late as it can be while it is still shown as soon as without; and the measure of the
// latency (Device::measureLatency), from each input read to the refresh its frame was first shown at.
//
// Our own, vendor-neutral (no NVIDIA Reflex or AMD Anti-Lag, nothing of LatencyFleX). The idea is the usual "just in
// time" frame start. The backend keeps the frames waiting to be shown to one (or two, Ahead), and then a frame waits on
// purpose before it reads its input, until the later of two times:
// - with vsync, the time by which it must be done to be shown at the refresh it is for (the backend tells it, where
//   it can: Direct3D 12 from its frame statistics; Vulkan does not), less the most the last 64 frames' work took (from
//   the input read to the card's end of the frame, as if the card had nothing of ours before it), less a margin;
// - the time the card will be done with the frames it was given already (the backend tells it), less the most the last
//   64 frames took from their input read to their hand-over, less the card's delay in starting a frame (Lead): a
//   frame handed over earlier would only wait for the card, and its input be older for it.
// The first is what counts where a frame's work fits well within the time between two refreshes; the second where it
// does not (the card sets the pace, the screen's refreshes do not), and without vsync. The margin doubles at once when
// a frame paced for its refresh was done too late for it (the card's end against the time it was to be done by, where
// the backend can tell the card's time in the processor's) or the screen showed it late (the compositor's part of the
// way, which a busy machine delays), up to where the just-in-time wait no longer waits at all; it shrinks slowly while
// none is, never below a floor (what it was when a frame was last too late, and a little more), which itself is let go
// of slowly after a long calm. Where the frames' work does not fit into the time between two refreshes one after
// the other, two may wait to be shown (Ahead: the processor's part of the next frame then overlaps the card's part of
// the one before, as without the limiter), so that it shows as many frames as without.
//
// The frame rate kept (Keep): fewer frames waiting absorb less of the card's uneven times, and a frame handed over
// just in time loses the rest of the card's time slice, where other programs share the card (measured on this busy
// machine, the card 85-100% used by others: 240 Hz 5.5-6.7 -> 7.6-9.5 ms a frame, Vulkan without vsync 2.2-9.2 ->
// 10.8-14.5). So while the frames do not keep up (without vsync, or with it slower than the refresh), the limiter
// now and then runs a window of frames the way without it ("as without": three ahead, no wait of its own) between
// two windows of its own, keeps to the way without where that was clearly faster than both, and looks again later
// (each wait twice as long as the one before). Where the card is ours, both come out alike: its own way stays.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace render::gfx {

// One frame's times (seconds, Now()'s clock): its input read, its hand-over to the card, the card's first and last
// timestamps in the processor's time (0: not known, where the device cannot tell the card's clock in the processor's),
// the card's own time for it (its last timestamp less its first; 0: not known), the time it was to be done by (with
// vsync, for the refresh it was begun for; 0: none) and whether its start was put off for that (paced).
struct LatencyFrame {
  double read = 0, handed = 0, cardBegin = 0, cardEnd = 0, card = 0, deadline = 0;
  bool paced = false;
};

class LatencyLimiter {
 public:
  // With vsync, the time between the pictures shown (the refresh's, or its multiple: 0 while not known, and taken as
  // not known when over a tenth of a second); without, 0.
  void Pace(bool vsync, double period) { vsync_ = vsync, period_ = vsync && period > 0 && period < 0.1 ? period : 0; }
  double Period() const { return period_; }

  // A frame the card has finished, in the order they were handed over.
  void Work(const LatencyFrame& f) {
    const double previousEnd = lastCardEnd_;
    lastCardEnd_ = f.cardEnd;
    if (f.read <= 0 || f.handed < f.read) return;
    const double gap = lastHanded_ > 0 ? f.handed - lastHanded_ : 0;  // (the time from the frame before: Keep's)
    lastHanded_ = f.handed;
    if (++seen_ <= kSkip) return;  // (the first frames after the start: shaders made, the card's clocks coming up)
    const double cpu = f.handed - f.read;
    double work = cpu + f.card, start = 0;
    if (f.cardBegin > 0 && f.cardEnd > f.cardBegin) {
      // (what the card did for others first, the compositor and other programs, and its own start after a hand-over,
      // counts; what it did for our frame before does not: the frame waited for that only because it began early,
      // and then it was not paced)
      const double free = std::max(f.handed, previousEnd);
      start = std::max(0.0, f.cardBegin - free);
      work = cpu + start + (f.cardEnd - f.cardBegin);
      timed_ = true;
    }
    if (work > 1 || cpu > 1) return;  // (a pause: the window moved, the program stopped in a debugger)
    work_[next_] = work, cpu_[next_] = cpu, card_[next_] = f.card, start_[next_] = std::min(start, kLongest);
    next_ = (next_ + 1) % kWork;
    if (count_ < kWork) ++count_;
    ++frames_;
    if (f.paced) ++paced_;
    // too late for the refresh it was paced for: the card's end after the time it was to be done by (and a little)
    if (timed_ && f.paced && f.deadline > 0 && f.cardEnd > 0) Outcome(f.cardEnd > f.deadline - kGuard);
    else if (timed_ && f.deadline > 0 && f.cardEnd > f.deadline && !f.paced && ahead_ == 1 && vsync_) {
      ++lateUnpaced_;  // (begun at once, and still too late: frames do not fit one after the other)
      TwoAhead();
    }
    Choose();
    Keep(gap);
  }
  // From the screen: a picture shown a refresh or more later than it was meant to be, or in time. A late one, where
  // one of the last frames begun was paced for its refresh, grows the margin as a frame too late by the card's clock
  // does (Work): the card may have been done in time and the picture still have come late (the compositor took it a
  // refresh later: on a busy machine its own work and ours come later than reckoned), and the margin is there for
  // that too. The same miss is not counted twice (the card's clock sees it a frame or two before the screen does).
  // Where the card's end is not known in the processor's time, a picture in time is what lets the margin shrink.
  // `exact`: the backend knows the refresh each picture was shown at (Direct3D 12's frame statistics); else (Vulkan's
  // present wait, whose times come unevenly) a late one only moves the margin where the card's end is not known.
  void Shown(bool late, bool exact) {
    if (!vsync_ || period_ <= 0) return;
    if (!late) {
      if (!timed_ && lastPaced_) Outcome(false);
      return;
    }
    ++shownLate_;
    if ((exact || !timed_) && (pacedStarts_ & 7) != 0 && starts_ - lateAt_ > 4) Outcome(true);
  }
  // When the frame to come begins: `deadline`, the time by which it must be done for the refresh it will be shown at
  // (with vsync; 0: not known); `cardFree`, when the card will be done with what it was given (0: not known). Never
  // before `now`, never more than two frames' time after it and never more than kLongest (so that a time reckoned
  // wrong costs a frame, never a stall). Paced() then says whether the deadline set it. While stepped back to the way
  // without the limiter (Keep), no wait at all.
  double StartAt(double now, double deadline, double cardFree) {
    double at = now;
    lastPaced_ = false;
    if (back_) deadline = cardFree = 0;
    if (count_ > 0 && vsync_ && period_ > 0 && deadline > 0) {
      const double v = deadline - Predicted() - margin_;
      if (v > at) at = v, lastPaced_ = true;
    }
    if (count_ > 0 && cardFree > 0) {
      const double c = cardFree - Most(cpu_) - Lead();
      if (c > at) at = c, lastPaced_ = false;
    }
    at = std::min(at, now + std::min(2 * std::max(period_, 1.0 / 60), kLongest));
    waited_ += at - now;
    ++starts_;
    pacedStarts_ = (pacedStarts_ << 1 | (lastPaced_ ? 1u : 0u)) & 0xffu;
    return at;
  }
  bool Paced() const { return lastPaced_; }
  // How many frames may wait to be shown: 1, or 2 where they do not fit into a frame's time one after the other; 3
  // without vsync, and while stepped back (Keep): as without the limiter (the backends then hold no frames back for
  // the screen).
  int Ahead() const { return back_ ? 3 : ahead_; }
  // Stepped back to the way without the limiter, as the frames came faster so (Keep).
  bool SteppedBack() const { return back_; }
  // The work expected of the next frame: the most of the last 64, and a twentieth more (the card's clocks drop while
  // it waits between frames).
  double Predicted() const { return Most(work_) * 1.05; }
  // The card's own time for a frame, for when the card will be free: the least of the last 64 (other programs' work
  // on the card falls between a frame's timestamps and stretches its time; reckoned longer than it is, the card would
  // be thought busy when it is not, and the frames begun late: with the middle of the 64, Vulkan without vsync ran at
  // 5.0-5.5 ms a frame against 3.2-4.4 off, on a card other programs kept half busy).
  double Card() const {
    if (count_ == 0) return 0;
    return *std::min_element(card_, card_ + count_);
  }
  // How long before the card is free a frame is handed over (the wait for the card, StartAt): the most of the last
  // 64 frames' delays from the card being free for them (their hand-over, or the end of the frame before) to their
  // start on it, at least kLead. That delay is the card's own start (some tenths of a millisecond) where the card is
  // ours; where other programs share it, their work, which then makes the lead long and the wait for the card short
  // (as without the limiter: fewer frames waiting there cost frames, Keep). A lead shorter than the start would
  // leave the card idle between frames (with kLead alone, 0.3 ms, frames came 3-9% slower without vsync).
  double Lead() const { return std::max(kLead, Most(start_)); }
  // The card's end of the newest frame finished (0: not known).
  double LastCardEnd() const { return lastCardEnd_; }
  double Margin() const { return margin_; }
  long Missed() const { return missed_; }

  // One line for the measure: how it ran.
  void Print(const char* who) const {
    fprintf(stderr,
            "render: %s: the latency limiter: %s, frames %.3f ms apart; at the end: margin %.2f ms, work expected %.2f ms (to the hand-over %.2f, "
            "the card's %.2f, its lead %.2f), %d ahead; %ld frames, %.0f%% paced for their refresh, waits %.2f ms on average; %ld too late for their refresh "
            "(%ld begun at once), %ld shown late by the screen's count; the frame rate kept: %ld tries of the way without (its time between "
            "frames %.3f of its own, on average), %ld kept to it, %ld frames so (%s at the end)\n",
            who, vsync_ ? "vsync" : "no vsync", period_ * 1000, margin_ * 1000, Predicted() * 1000, Most(cpu_) * 1000, Card() * 1000, Lead() * 1000, Ahead(), frames_,
            frames_ ? 100.0 * static_cast<double>(paced_) / static_cast<double>(frames_) : 0.0, starts_ ? 1000 * waited_ / static_cast<double>(starts_) : 0.0,
            missed_, lateUnpaced_, shownLate_, tries_, tried_ ? tryRatios_ / static_cast<double>(tried_) : 0.0, kept_, framesBack_,
            back_ ? "the way without" : "its own way");
  }

 private:
  // A frame paced for its refresh: too late for it, or in time. Too late: the margin twice what it was (at least kJump
  // more), up to where the just-in-time wait no longer waits at all (a frame then begins as soon as the frames before
  // allow, as with the frame latency alone: misses from a busy machine soon leave the limiter no worse than that).
  void Outcome(bool late) {
    const double most = std::max({kMost, 0.25 * period_, period_ - Predicted()});
    if (late) {
      ++missed_;
      lateAt_ = starts_;
      floor_ = std::min(std::max(floor_, margin_ + kAbove), most);
      margin_ = std::min(std::max(2 * margin_, margin_ + kJump), most);
      calm_ = 0;
      return;
    }
    margin_ = std::max(floor_, margin_ - kCreep);
    if (++calm_ >= kCalm) {  // (a tenth of the floor let go of each time, at least kLetGo: a high one goes sooner)
      floor_ = std::max(kLeast, floor_ - std::max(kLetGo, 0.1 * floor_));
      calm_ = 0;
    }
  }
  // One ahead or two: two at once after two frames whose work and margin do not fit into a frame's time; one again
  // after a while (kRetry frames, twice as long each time it had to go two ahead again soon after) in which they fit
  // well. Without vsync, three.
  void Choose() {
    if (++sinceAhead_ > 3600) retry_ = kRetry;  // (a minute or so at one ahead: trying again soon is fine again)
    if (!vsync_) {  // (three, as without the limiter: a window the compositor shows takes a frame from its queue only
                    // at each refresh, so fewer would hold the frames to so many a refresh; the backends hold none
                    // back for the screen without vsync, only the wait for the card paces them)
      ahead_ = 3;
      return;
    }
    if (ahead_ == 3) ahead_ = 2, under_ = 0;  // (vsync on again)
    if (period_ <= 0) return;
    // (the margin counted as at most a quarter of a frame's time, or kMost: more is there for the screen's misses, not
    // for the frames' work, and frames two ahead would not mend those)
    const double need = Predicted() + std::min(margin_, std::max(kMost, 0.25 * period_));
    if (ahead_ == 1) {
      over_ = need > period_ ? over_ + 1 : 0;
      if (over_ >= 2) TwoAhead();
    } else {
      under_ = need < 0.85 * period_ ? under_ + 1 : 0;
      if (under_ >= retry_) ahead_ = 1, over_ = 0, sinceAhead_ = 0;
    }
  }
  void TwoAhead() {
    ahead_ = 2, under_ = 0;
    if (sinceAhead_ < 3600) retry_ = std::min(retry_ * 2, 16 * kRetry);
    sinceAhead_ = 0;
  }
  // The frame rate kept: the time between the frames' hand-overs, averaged over windows of kWindow frames (the first
  // kSettle after each change left out: the frames waiting fill up or drain). Each comparison is of one way's window
  // against the other's on both sides of it, so that frames growing faster or slower meanwhile (the start, the scene)
  // do not decide it. With its own way (kOwn), when the frames do not keep up (without vsync, or with it a tenth slower
  // than the refresh) and the last try is long enough ago: a window the way without (kTry), then its own again
  // (kOwnAgain). The way without faster than both by over kGain: kept to it (kBack) for a while; then its own for a
  // window (kAgain) and the way without again (kConfirm): its own slower than both by as much, back for twice as long;
  // else its own way. The next try twice as late each time (up to kTryWaitMost frames).
  void Keep(double gap) {
    if (gap <= 0 || gap > 0.1) return;  // (a pause, or the first frame)
    if (back_) ++framesBack_;
    ++sinceTry_;
    if (settle_ > 0) {
      --settle_;
      return;
    }
    windowSum_ += gap;
    if (++windowFrames_ < kWindow) return;
    const double mean = windowSum_ / windowFrames_;
    windowSum_ = 0, windowFrames_ = 0;
    auto to = [&](Phase p, bool back) {
      if (back != back_) settle_ = kSettle;
      phase_ = p, back_ = back;
    };
    switch (phase_) {
      case kOwn:
        if ((!vsync_ || (period_ > 0 && mean > 1.1 * period_)) && sinceTry_ >= tryWait_) {
          ownMean_ = mean, to(kTry, true);
          ++tries_;
        }
        break;
      case kTry:
        backMean_ = mean, to(kOwnAgain, false);
        break;
      case kOwnAgain:
        tryRatios_ += backMean_ / (0.5 * (ownMean_ + mean)), ++tried_;  // (the measure's: the way without against its own)
        if (backMean_ < kGain * std::min(ownMean_, mean)) {
          to(kBack, true), backLeft_ = backFor_;
          ++kept_;
        } else {
          to(kOwn, false), sinceTry_ = 0, tryWait_ = std::min(2 * tryWait_, kTryWaitMost);
        }
        break;
      case kBack:
        backMean_ = mean;
        if ((backLeft_ -= kWindow) <= 0) to(kAgain, false);
        break;
      case kAgain:
        ownMean_ = mean, to(kConfirm, true);
        break;
      case kConfirm:
        if (kGain * ownMean_ > std::max(backMean_, mean)) {  // (its own way slower again: back, for twice as long)
          backMean_ = mean, to(kBack, true);
          backFor_ = std::min(2 * backFor_, kBackForMost), backLeft_ = backFor_;
        } else {
          to(kOwn, false), sinceTry_ = 0, backFor_ = kBackFor, tryWait_ = std::min(2 * tryWait_, kTryWaitMost);
        }
        break;
    }
  }
  double Most(const double* w) const {
    double most = 0;
    for (int i = 0; i < count_; ++i) most = std::max(most, w[i]);
    return most;
  }
  static constexpr int kWork = 64;
  static constexpr int kSkip = 20;           // frames not counted after the start
  static constexpr int kRetry = 120;         // frames whose work fits well before one ahead is tried again
  static constexpr double kLead = 0.0003;    // a frame handed over at least this much before the card is free (s)
  static constexpr double kGuard = 0.0002;   // done less than this before the time it was to be done by: too late
  static constexpr double kLongest = 0.05;   // the longest a frame's start is put off (s)
  static constexpr double kLeast = 0.0005;   // the margin's least (s)
  static constexpr double kMost = 0.004;     // its most at least (or a quarter of a frame, or what leaves no wait)
  static constexpr double kJump = 0.0015;    // added at once when a frame was too late
  static constexpr double kAbove = 0.0005;   // the floor: the margin that was too little, and this
  static constexpr double kCreep = 0.00001;  // taken off for each frame in time
  static constexpr double kLetGo = 0.0001;   // taken off the floor ...
  static constexpr int kCalm = 600;          // ... after this many frames in time
  static constexpr int kWindow = 120;        // Keep: frames a window
  static constexpr int kSettle = 8;          // ... left out after a change
  static constexpr double kGain = 0.93;      // ... the way faster by this much (its time between frames) is kept
  static constexpr int kTryWait = 360;       // ... frames before the first try of the way without
  static constexpr int kTryWaitMost = 5760;  // ... doubling after each try, to this
  static constexpr int kBackFor = 600;       // ... frames kept to the way without before its own is tried again
  static constexpr int kBackForMost = 9600;  // ... doubling each time its own was slower again, to this
  enum Phase { kOwn, kTry, kOwnAgain, kBack, kAgain, kConfirm };
  Phase phase_ = kOwn;
  bool back_ = false;
  int windowFrames_ = 0, settle_ = 0, sinceTry_ = 0, tryWait_ = kTryWait, backFor_ = kBackFor, backLeft_ = 0;
  double windowSum_ = 0, ownMean_ = 0, backMean_ = 0, lastHanded_ = 0;
  long tries_ = 0, kept_ = 0, framesBack_ = 0;
  // (the measure's: each try's time between frames the way without, over its own way's on both sides, summed; tries done)
  double tryRatios_ = 0;
  long tried_ = 0;
  double work_[kWork] = {}, cpu_[kWork] = {}, card_[kWork] = {}, start_[kWork] = {};
  int next_ = 0, count_ = 0, seen_ = 0;
  bool vsync_ = true, timed_ = false, lastPaced_ = false;
  double period_ = 0, lastCardEnd_ = 0, waited_ = 0;
  double margin_ = 0.001, floor_ = kLeast;
  int calm_ = 0, over_ = 0, under_ = 0, ahead_ = 1, retry_ = kRetry, sinceAhead_ = 0;
  long missed_ = 0, lateUnpaced_ = 0, shownLate_ = 0, frames_ = 0, paced_ = 0, starts_ = 0;
  long lateAt_ = -100;          // (starts_ at the last miss counted)
  unsigned pacedStarts_ = 0;    // (the last eight starts, the newest lowest: whether each was paced for its refresh)
};

// The measure (Device::measureLatency): each frame handed over to be shown, by the device's own number for its
// showing (rising by one for each picture shown) and the time its input was read; each showing the device learns
// of, with the time of the refresh it was first shown at. Printed as one line: the latency's average, middle, 99th
// hundredth and worst, and the gaps between pictures shown one after the other (how evenly they came, on the
// screen itself: their mean, spread and largest, and with vsync how many came a refresh or more later than the time
// between pictures). The first 60 pictures are not counted (the start: shaders made, the limiter learning).
class LatencyRecord {
 public:
  void Presented(uint64_t id, double readAt) {
    ring_[next_] = {id, readAt};
    next_ = (next_ + 1) % kRing;
  }
  void Shown(uint64_t id, double at) {
    if (++seen_ <= 60) {
      lastId_ = id, lastAt_ = at;
      return;
    }
    for (const Entry& e : ring_)
      if (e.id == id && e.read > 0 && at > e.read) {
        latencies_.push_back(static_cast<float>((at - e.read) * 1000));
        break;
      }
    if (lastId_ && id == lastId_ + 1 && at > lastAt_) gaps_.push_back(static_cast<float>((at - lastAt_) * 1000));
    // (more: a new swap chain)
    else if (lastId_ && id > lastId_ + 1 && id - lastId_ < 1000) unseen_ += static_cast<long>(id - lastId_ - 1);
    lastId_ = id, lastAt_ = at;
  }
  // `period`: with vsync, the time between pictures (s; the refresh's times the sync interval), `refresh` the
  // refresh's: the pictures shown more than half a refresh later than `period` after the one before are counted
  // (0: not counted)
  void Print(const char* who, double period, double refresh) const {
    if (latencies_.empty()) {
      fprintf(stderr, "render: %s: latency: no frame's showing was seen (%ld)\n", who, seen_);
      return;
    }
    std::vector<float> l = latencies_;
    std::sort(l.begin(), l.end());
    double sum = 0;
    for (float v : l) sum += v;
    double gapSum = 0, gapSquares = 0, gapMost = 0;
    long late = 0;
    for (float g : gaps_) {
      gapSum += g, gapSquares += static_cast<double>(g) * g, gapMost = std::max(gapMost, static_cast<double>(g));
      if (period > 0 && g > 1000 * (period + 0.5 * refresh)) ++late;
    }
    const double gapMean = gaps_.empty() ? 0 : gapSum / static_cast<double>(gaps_.size());
    const double gapSpread = gaps_.empty() ? 0 : std::sqrt(std::max(0.0, gapSquares / static_cast<double>(gaps_.size()) - gapMean * gapMean));
    fprintf(stderr,
            "render: %s: latency, input read to first shown: %zu frames, average %.2f ms, middle %.2f, 99%% %.2f, worst %.2f; "
            "gaps between pictures shown: %zu, mean %.2f ms, spread %.2f, largest %.2f, %ld late by a refresh or more (%ld not seen)\n",
            who, l.size(), sum / static_cast<double>(l.size()), l[l.size() / 2], l[std::min(l.size() - 1, l.size() * 99 / 100)], l.back(),
            gaps_.size(), gapMean, gapSpread, gapMost, late, unseen_);
  }

 private:
  static constexpr int kRing = 64;
  struct Entry {
    uint64_t id = 0;
    double read = 0;
  };
  Entry ring_[kRing];
  int next_ = 0;
  long seen_ = 0, unseen_ = 0;
  uint64_t lastId_ = 0;
  double lastAt_ = 0;
  std::vector<float> latencies_, gaps_;
};

// What the limiter's own part of a frame took on the processor (the measure's; seconds, summed over the frames): the
// frames' times taken from the card, the swap chain's or the driver's statistics and the rest of the reckoning, and its
// waits (for the frames before to be shown, and just in time). Printed as one line, each a frame's average.
class LatencyCost {
 public:
  void Add(double work, double reckoning, double shownWait, double justInTime) {
    work_ += work, reckoning_ += reckoning, shownWait_ += shownWait, justInTime_ += std::max(0.0, justInTime);
    ++frames_;
  }
  void Print(const char* who) const {
    if (frames_ == 0) return;
    const double n = static_cast<double>(frames_) / 1000;
    fprintf(stderr,
            "render: %s: the latency limiter's own work on the processor %.3f ms a frame (the card's times %.3f, the rest %.3f); "
            "its waits %.3f ms a frame for the frames before, %.3f just in time (%ld frames)\n",
            who, (work_ + reckoning_) / n, work_ / n, reckoning_ / n, shownWait_ / n, justInTime_ / n, frames_);
  }

 private:
  double work_ = 0, reckoning_ = 0, shownWait_ = 0, justInTime_ = 0;
  long frames_ = 0;
};

// The pictures' showings as a device learns them one by one (Vulkan's present wait: a picture's number and when it
// was shown): the time between two pictures shown one after the other (the middle of the last 31: with vsync, the
// refresh's, a missed refresh or two left out), the newest picture known and when it was shown, and whether the
// newest came a refresh late (a gap of more than one and a half).
class ShowingTimes {
 public:
  // false when it is not newer than what is known
  bool Shown(uint64_t id, double at) {
    if (id <= newest_) return false;
    late_ = false;
    if (newest_ && id == newest_ + 1 && at > newestAt_) {
      const double gap = at - newestAt_;
      late_ = count_ >= kGaps && gap > 1.5 * Period();
      gaps_[next_] = gap;
      next_ = (next_ + 1) % kGaps;
      if (count_ < kGaps) ++count_;
    }
    newest_ = id, newestAt_ = at;
    return true;
  }
  double Period() const {
    if (count_ < kGaps) return 0;
    double g[kGaps];
    std::copy(gaps_, gaps_ + kGaps, g);
    std::nth_element(g, g + kGaps / 2, g + kGaps);
    return g[kGaps / 2];
  }
  uint64_t Newest() const { return newest_; }
  double NewestAt() const { return newestAt_; }
  bool Late() const { return late_; }
  void Reset() { *this = ShowingTimes{}; }

 private:
  static constexpr int kGaps = 31;
  double gaps_[kGaps] = {};
  int next_ = 0, count_ = 0;
  uint64_t newest_ = 0;
  double newestAt_ = 0;
  bool late_ = false;
};

// The screen's refreshes as a device learns them from refreshes it is told the time of (their numbers rising by one
// for each): the time between them over a long run (begun again every 1200 refreshes or so, from the last one known,
// so that a window moved to a screen of another rate is soon paced for it), and the last one's time.
class RefreshGrid {
 public:
  void Refresh(uint64_t number, double at) {
    if (!first_ || number < first_ || number - first_ > 20000) first_ = number, firstAt_ = at;
    else if (number - first_ >= 30) {
      const double p = (at - firstAt_) / static_cast<double>(number - first_);
      if (p > 0.002 && p < 0.1) period_ = p;
      if (number - first_ >= 1200 && last_ > first_ && number > last_) first_ = last_, firstAt_ = lastAt_;
    }
    last_ = number, lastAt_ = at;
  }
  double Period() const { return period_; }

 private:
  uint64_t first_ = 0, last_ = 0;
  double firstAt_ = 0, lastAt_ = 0, period_ = 0;
};

}  // namespace render::gfx
