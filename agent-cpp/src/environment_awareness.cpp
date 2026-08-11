#include "environment_awareness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace aoi {

namespace {

std::string nowTimeStr() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &tt);
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return buf;
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string msToTimeStr(int64_t epochMs) {
  const auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(epochMs));
  const auto tt = std::chrono::system_clock::to_time_t(tp);
  std::tm tm{};
  localtime_s(&tm, &tt);
  char buf[32];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return buf;
}

std::vector<std::string> listFrameFiles(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return out;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("frame_", 0) != 0) continue;
    if (name.size() >= 4 &&
        (name.compare(name.size() - 4, 4, ".jpg") == 0 ||
         name.compare(name.size() - 4, 4, ".png") == 0)) {
      out.push_back(name);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

EnvironmentAwareness::EnvironmentAwareness(EnvironmentAwarenessOptions opts)
    : opts_(std::move(opts)) {
  if (opts_.describeFrame == nullptr) {
    opts_.describeFrame = [](const std::string&) { return std::string("(描述未配置)"); };
  }
  if (opts_.framesDir.empty()) {
    const char* env = getenv("AOI_FRAMES_DIR");
    if (env) {
      opts_.framesDir = env;
    } else {
      opts_.framesDir = "context/frames";
    }
  }
  framesDir_ = opts_.framesDir;
  std::error_code ec;
  fs::create_directories(framesDir_, ec);
  audioDir_ = opts_.audioDir.empty() ? (framesDir_ + "/../audio") : opts_.audioDir;
  fs::create_directories(audioDir_, ec);
}

EnvironmentAwareness::~EnvironmentAwareness() { stop(); }

void EnvironmentAwareness::start() {
  if (enabled_.exchange(true)) return;
  enabled = true;  // public flag mirrors enabled_ (agent checks this)
  visualThread_ = std::thread([this] { visualLoop(); });
  cleanupThread_ = std::thread([this] { cleanupLoop(); });
  if (opts_.captureAudio) startAudio();
}

void EnvironmentAwareness::stop() {
  if (!enabled_.exchange(false)) return;
  enabled = false;
  stopCv_.notify_all();
  if (visualThread_.joinable()) visualThread_.join();
  if (cleanupThread_.joinable()) cleanupThread_.join();
  stopAudio();
}

void EnvironmentAwareness::visualLoop() {
  while (enabled_.load()) {
    processVisualBatch();
    std::this_thread::sleep_for(std::chrono::seconds(opts_.batchSeconds));
  }
}

void EnvironmentAwareness::cleanupLoop() {
  while (enabled_.load()) {
    // Interruptible wait: stop() notifies stopCv_ so join() returns promptly
    // instead of blocking for the full interval.
    {
      std::unique_lock<std::mutex> lk(stopMutex_);
      stopCv_.wait_for(lk, std::chrono::seconds(30),
                       [this]() { return !enabled_.load(); });
    }
    if (!enabled_.load()) break;
    const int64_t cutoff = nowMs() - static_cast<int64_t>(opts_.windowMinutes) * 60 * 1000;
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<VisualEntry> expired;
    std::vector<AudioEntry> expiredAudio;
    for (const auto& e : visualEntries_) {
      if (e.time < cutoff) expired.push_back(e);
    }
    for (const auto& a : audioEntries_) {
      if (a.time < cutoff) expiredAudio.push_back(a);
    }
    if (!expired.empty() || !expiredAudio.empty()) {
      std::vector<std::string> parts;
      for (const auto& e : expired) {
        if (!e.desc.empty()) parts.push_back(e.desc);
      }
      for (const auto& a : expiredAudio) {
        if (!a.text.empty()) parts.push_back(a.text);
      }
      if (!parts.empty()) {
        const int64_t t0 = expired.empty() ? expiredAudio[0].time : expired[0].time;
        SummaryEntry s;
        s.timeStart = t0;
        s.timeEnd = nowMs();
        std::string joined;
        const size_t from = parts.size() > 20 ? parts.size() - 20 : 0;
        for (size_t i = from; i < parts.size(); i++) {
          if (!joined.empty()) joined += "; ";
          joined += parts[i];
        }
        s.text = joined;
        summaries_.push_back(s);
        if (summaries_.size() > 50) summaries_.erase(summaries_.begin());
      }
      const int64_t c2 = cutoff;
      auto v2 = visualEntries_;
      v2.erase(std::remove_if(v2.begin(), v2.end(),
                              [&](const VisualEntry& e) { return e.time < c2; }),
               v2.end());
      visualEntries_ = std::move(v2);
      auto a2 = audioEntries_;
      a2.erase(std::remove_if(a2.begin(), a2.end(),
                              [&](const AudioEntry& a) { return a.time < c2; }),
               a2.end());
      audioEntries_ = std::move(a2);
      // Delete expired frame files.
      for (const auto& e : expired) {
        std::error_code ec;
        fs::remove(e.framePath, ec);
      }
    }
  }
}

std::string EnvironmentAwareness::getNewestFramePath() const {
  // Lock: cleanupLoop deletes expired frame files concurrently; without the
  // lock we could return a path that is being removed (callers already fall
  // back on read failure, but a stale path is better avoided).
  std::lock_guard<std::mutex> lk(mutex_);
  const auto files = listFrameFiles(framesDir_);
  if (files.empty()) return "";
  return framesDir_ + "/" + files.back();
}

std::string EnvironmentAwareness::getNewestClipPath() const {
  // Prefer the in-memory list, else scan the audio dir (pre-seeded files).
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!audioClips_.empty()) return audioClips_.back().path;
  }
  std::error_code ec;
  if (!fs::exists(audioDir_, ec)) return "";
  std::vector<std::string> wavs;
  for (const auto& entry : fs::directory_iterator(audioDir_, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("seg_", 0) == 0 && name.size() > 4 &&
        name.compare(name.size() - 4, 4, ".wav") == 0) {
      wavs.push_back(entry.path().string());
    }
  }
  std::sort(wavs.begin(), wavs.end());
  return wavs.empty() ? "" : wavs.back();
}

std::string EnvironmentAwareness::listClips(int minutes) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (audioClips_.empty()) return "";
  const int64_t cutoff = minutes > 0
      ? nowMs() - static_cast<int64_t>(minutes) * 60 * 1000
      : 0;
  std::string out;
  for (const auto& c : audioClips_) {
    if (c.time < cutoff) continue;
    if (!out.empty()) out += "\n";
    out += c.timeStr + " " + std::to_string(c.durationMs / 1000) + "s " + c.path;
  }
  return out;
}

void EnvironmentAwareness::processVisualBatch() {
  if (!enabled_.load()) return;
  if (inFlight_.load() >= opts_.concurrency) return;  // mirror JS inFlight limit
  const auto files = listFrameFiles(framesDir_);
  if (files.empty()) return;
  const std::string newest = files.back();
  const std::string path = framesDir_ + "/" + newest;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!visualEntries_.empty() && visualEntries_.back().framePath == path) return;
  }

  if (!opts_.autoUnderstand) {
    // No auto-understanding: just record the frame (AI reads it on demand via
    // get_context/screenshot). No LLM call.
    std::lock_guard<std::mutex> lk(mutex_);
    VisualEntry v;
    v.time = nowMs();
    v.timeStr = nowTimeStr();
    v.framePath = path;
    v.desc = "";
    v.diff = "";
    v.kind = "frame";
    visualEntries_.push_back(v);
    // Cap the list (frames are deleted by cleanupLoop by window age).
    return;
  }

  inFlight_++;
  std::string desc;
  try {
    desc = opts_.describeFrame(path);
  } catch (const std::exception& ex) {
    desc = "(describe error: " + std::string(ex.what()) + ")";
  } catch (...) {
    desc = "(describe error)";
  }
  inFlight_--;
  const std::string diff = computeDiff(lastDesc_, desc);
  lastDesc_ = desc;

  std::lock_guard<std::mutex> lk(mutex_);
  VisualEntry v;
  v.time = nowMs();
  v.timeStr = nowTimeStr();
  v.framePath = path;
  v.desc = desc;
  v.diff = diff;
  v.kind = "working";
  visualEntries_.push_back(v);
}

std::string EnvironmentAwareness::computeDiff(const std::string& prev,
                                              const std::string& cur) const {
  if (prev.empty()) return "initial view";
  if (prev == cur) return "unchanged";
  // Split on whitespace/punctuation; collect added words (up to 8).
  std::set<std::string> prevWords;
  std::string w;
  // Match both ASCII and UTF-8 multi-byte CJK punctuation so the split works
  // under clang-cl (which rejects multi-byte char literals) and MSVC alike.
  size_t i = 0;
  auto isSepAt = [&](const std::string &s, size_t pos) -> bool {
    if (pos >= s.size()) return false;
    const unsigned char c = (unsigned char)s[pos];
    if (c == ' ' || c == ',' || c == ';' || c == ':') return true;
    if (pos + 2 >= s.size()) return false;
    const unsigned char b1 = (unsigned char)s[pos + 1];
    const unsigned char b2 = (unsigned char)s[pos + 2];
    if (c == 0xEF && b1 == 0xBC)
      return (b2 == 0x8C || b2 == 0x81 || b2 == 0x9A || b2 == 0x9F);  // ，！：？
    if (c == 0xE3 && b1 == 0x80)
      return (b2 == 0x82 || b2 == 0x81);  // 。、
    return false;
  };
  while (i < prev.size()) {
    if (isSepAt(prev, i)) {
      if (!w.empty()) { prevWords.insert(w); w.clear(); }
      i += 3;
    } else {
      w += prev[i];
      i += 1;
    }
  }
  if (!w.empty()) prevWords.insert(w);

  std::vector<std::string> added;
  w.clear();
  i = 0;
  while (i < cur.size()) {
    if (isSepAt(cur, i)) {
      if (!w.empty()) {
        if (prevWords.find(w) == prevWords.end() && added.size() < 8) added.push_back(w);
        w.clear();
      }
      i += 3;
    } else {
      w += cur[i];
      i += 1;
    }
  }
  if (!w.empty() && prevWords.find(w) == prevWords.end() && added.size() < 8) added.push_back(w);

  if (!added.empty()) {
    std::string out = "新增: ";
    for (size_t i = 0; i < added.size(); i++) {
      if (i) out += " ";
      out += added[i];
    }
    return out;
  }
  return "场景变化";
}

void EnvironmentAwareness::startAudio() {
  std::lock_guard<std::mutex> lk(audioMutex_);
  if (audioSegmenter_ && audioSegmenter_->running()) return;
  SpeechSegmenterOptions segOpts;
  segOpts.fixedSegmentSeconds = opts_.autoUnderstand ? 0 : opts_.audioSegmentSeconds;
  segOpts.onSegment = [this](const SpeechSegment& seg) {
    if (opts_.autoUnderstand) {
      // Auto-understand: transcribe as before.
      if (!seg.text.empty()) addAudio(seg.text);
      return;
    }
    // Fixed 30s raw segment: store wav file, record the clip.
    if (seg.wavBuffer.size() <= 44) return;
    std::error_code ec;
    const std::string fname = "seg_" + std::to_string(seg.seq) + "_" +
                              std::to_string(seg.startSample) + ".wav";
    const std::string path = audioDir_ + "/" + fname;
    {
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (f) f.write(reinterpret_cast<const char*>(seg.wavBuffer.data()),
                     static_cast<std::streamsize>(seg.wavBuffer.size()));
    }
    {
      std::lock_guard<std::mutex> lk(mutex_);
      AudioClipEntry c;
      c.time = nowMs();
      c.timeStr = nowTimeStr();
      c.path = path;
      c.durationMs = seg.durationMs;
      c.seq = seg.seq;
      audioClips_.push_back(c);
      // Trim old clips beyond the window.
      const int64_t cutoff = nowMs() - static_cast<int64_t>(opts_.windowMinutes) * 60 * 1000;
      while (!audioClips_.empty() && audioClips_.front().time < cutoff) {
        fs::remove(audioClips_.front().path, ec);
        audioClips_.erase(audioClips_.begin());
      }
    }
  };
  segOpts.onState = [](const std::string& state, const std::string& msg) {
    if (state == "error") {
      // Non-fatal: log and continue.
    }
  };
  audioSegmenter_ = std::make_unique<SpeechSegmenter>(std::move(segOpts));
  audioSegmenter_->start();
}

void EnvironmentAwareness::stopAudio() {
  std::lock_guard<std::mutex> lk(audioMutex_);
  if (audioSegmenter_) {
    audioSegmenter_->stop();
    audioSegmenter_.reset();
  }
}

void EnvironmentAwareness::pauseAudio() {
  std::lock_guard<std::mutex> lk(audioMutex_);
  if (audioSegmenter_ && audioSegmenter_->running()) {
    audioSegmenter_->stop();
    audioSegmenter_.reset();
  }
}

void EnvironmentAwareness::resumeAudio() {
  if (!enabled_.load() || !opts_.captureAudio) return;
  startAudio();  // startAudio() takes audioMutex_ itself
}

void EnvironmentAwareness::addAudio(const std::string& text) {
  if (text.empty()) return;
  std::lock_guard<std::mutex> lk(mutex_);
  AudioEntry a;
  a.time = nowMs();
  a.timeStr = nowTimeStr();
  a.text = text;
  a.kind = "working";
  audioEntries_.push_back(a);
}

std::string EnvironmentAwareness::getContext(int minutes) const {
  const int windowMin = minutes > 0 ? minutes : opts_.windowMinutes;
  const int64_t cutoff = nowMs() - static_cast<int64_t>(windowMin) * 60 * 1000;

  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<VisualEntry> vis;
  std::vector<AudioEntry> aud;
  for (const auto& v : visualEntries_) {
    if (v.time >= cutoff) vis.push_back(v);
  }
  for (const auto& a : audioEntries_) {
    if (a.time >= cutoff) aud.push_back(a);
  }
  const size_t summFrom = summaries_.size() > 3 ? summaries_.size() - 3 : 0;

  std::string out;
  if (summaries_.size() > 0) {
    out += "【长期记忆】\n";
    for (size_t i = summFrom; i < summaries_.size(); i++) {
      out += "  " + msToTimeStr(summaries_[i].timeStart) + ": " + summaries_[i].text + "\n";
    }
  }
  if (!vis.empty()) {
    out += "【视觉时间线】\n";
    const size_t from = vis.size() > 30 ? vis.size() - 30 : 0;
    for (size_t i = from; i < vis.size(); i++) {
      out += "  " + vis[i].timeStr;
      if (!vis[i].diff.empty() && vis[i].diff != "unchanged") out += " [" + vis[i].diff + "]";
      out += " " + vis[i].desc + "\n";
    }
  }
  if (!aud.empty()) {
    out += "【听觉内容】\n";
    const size_t from = aud.size() > 30 ? aud.size() - 30 : 0;
    for (size_t i = from; i < aud.size(); i++) {
      out += "  " + aud[i].timeStr + ": " + aud[i].text + "\n";
    }
  }
  if (out.empty()) return "(环境感知开启中,暂无上下文)";
  return out;
}

} // namespace aoi
