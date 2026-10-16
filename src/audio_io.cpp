#include "easyai/audio_io.hpp"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#define EASYAI_AUDIO_COREAUDIO 1
#elif defined(EASYAI_HAVE_ALSA)
#include <alsa/asoundlib.h>
#define EASYAI_AUDIO_ALSA 1
#endif

namespace easyai {
namespace audio {

namespace {
constexpr int kCaptureBuffers    = 4;      // queued periods in flight (CoreAudio) / buffer periods (ALSA)
constexpr int kMaxQueuedPeriods  = 100;    // 2 s at 20 ms: beyond this the oldest periods are dropped
constexpr int kPlayChunkMs       = 250;    // playback buffer size
constexpr int kMsPerSecond       = 1000;
}  // namespace

std::string backend_name() {
#if defined(EASYAI_AUDIO_COREAUDIO)
    return "coreaudio";
#elif defined(EASYAI_AUDIO_ALSA)
    return "alsa";
#else
    return "none";
#endif
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

struct Capture::Impl {
    int         rate   = 0;
    int         period = 0;
    bool        opened = false;
    std::string error;
#if defined(EASYAI_AUDIO_COREAUDIO)
    AudioQueueRef                          queue = nullptr;
    std::mutex                             mu;
    std::condition_variable                cv;
    std::deque<std::vector<std::int16_t>>  ready;   // whole periods, oldest first
    bool                                   failed = false;

    static void on_input(void * user, AudioQueueRef q, AudioQueueBufferRef buf,
                         const AudioTimeStamp *, UInt32 n_packets, const AudioStreamPacketDescription *) {
        auto * self = static_cast<Impl *>(user);
        if (n_packets > 0) {
            std::vector<std::int16_t> v((std::size_t) n_packets);
            std::memcpy(v.data(), buf->mAudioData, n_packets * sizeof(std::int16_t));
            std::lock_guard<std::mutex> lk(self->mu);
            self->ready.push_back(std::move(v));
            while ((int) self->ready.size() > kMaxQueuedPeriods) self->ready.pop_front();
            self->cv.notify_one();
        }
        if (self->opened) AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    }
#elif defined(EASYAI_AUDIO_ALSA)
    snd_pcm_t * pcm = nullptr;
#endif
};

Capture::Capture() : p_(new Impl) {}
Capture::~Capture() { close(); }
bool Capture::is_open() const { return p_->opened; }
int  Capture::rate_hz() const { return p_->rate; }
int  Capture::period_frames() const { return p_->period; }
const std::string & Capture::last_error() const { return p_->error; }

bool Capture::open(const std::string & device, int rate_hz, int period_ms, std::string & err) {
    close();
    p_->rate   = rate_hz;
    p_->period = rate_hz * period_ms / kMsPerSecond;
#if defined(EASYAI_AUDIO_COREAUDIO)
    (void) device;   // CoreAudio: the system default input
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = rate_hz;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mChannelsPerFrame = 1;
    fmt.mBytesPerFrame    = 2;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = 2;
    OSStatus st = AudioQueueNewInput(&fmt, Impl::on_input, p_.get(), nullptr, nullptr, 0, &p_->queue);
    if (st != noErr) { err = "AudioQueueNewInput failed (" + std::to_string(st) + ")"; return false; }
    p_->opened = true;
    for (int i = 0; i < kCaptureBuffers; ++i) {
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(p_->queue, (UInt32) (p_->period * sizeof(std::int16_t)), &buf) != noErr ||
            AudioQueueEnqueueBuffer(p_->queue, buf, 0, nullptr) != noErr) {
            err = "AudioQueue buffer setup failed"; close(); return false;
        }
    }
    st = AudioQueueStart(p_->queue, nullptr);
    if (st != noErr) { err = "AudioQueueStart failed (" + std::to_string(st) + ") — microphone permission?"; close(); return false; }
    return true;
#elif defined(EASYAI_AUDIO_ALSA)
    int rc = snd_pcm_open(&p_->pcm, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0) { err = std::string("snd_pcm_open(") + device + "): " + snd_strerror(rc); return false; }
    unsigned rate = (unsigned) rate_hz;
    snd_pcm_uframes_t period = (snd_pcm_uframes_t) p_->period;
    snd_pcm_uframes_t buffer = period * kCaptureBuffers;
    snd_pcm_hw_params_t * hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    rc = snd_pcm_hw_params_any(p_->pcm, hw);
    if (rc >= 0) rc = snd_pcm_hw_params_set_access(p_->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc >= 0) rc = snd_pcm_hw_params_set_format(p_->pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (rc >= 0) rc = snd_pcm_hw_params_set_channels(p_->pcm, hw, 1);
    if (rc >= 0) rc = snd_pcm_hw_params_set_rate_near(p_->pcm, hw, &rate, nullptr);
    if (rc >= 0) rc = snd_pcm_hw_params_set_period_size_near(p_->pcm, hw, &period, nullptr);
    if (rc >= 0) rc = snd_pcm_hw_params_set_buffer_size_near(p_->pcm, hw, &buffer);
    if (rc >= 0) rc = snd_pcm_hw_params(p_->pcm, hw);
    if (rc < 0) { err = std::string("ALSA capture hw params: ") + snd_strerror(rc); close(); return false; }
    if ((int) rate != rate_hz) { err = "ALSA device cannot capture at " + std::to_string(rate_hz) + " Hz (use a plug: device, e.g. plughw:1,0)"; close(); return false; }
    p_->period = (int) period;
    rc = snd_pcm_prepare(p_->pcm);
    if (rc < 0) { err = std::string("snd_pcm_prepare: ") + snd_strerror(rc); close(); return false; }
    p_->opened = true;
    return true;
#else
    (void) device; (void) rate_hz; (void) period_ms;
    err = "built without an audio backend (ALSA headers missing at build time)";
    return false;
#endif
}

void Capture::close() {
#if defined(EASYAI_AUDIO_COREAUDIO)
    if (p_->queue) {
        p_->opened = false;
        AudioQueueStop(p_->queue, true);
        AudioQueueDispose(p_->queue, true);
        p_->queue = nullptr;
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->ready.clear();
    }
#elif defined(EASYAI_AUDIO_ALSA)
    if (p_->pcm) { snd_pcm_close(p_->pcm); p_->pcm = nullptr; }
#endif
    p_->opened = false;
}

void Capture::flush() {
#if defined(EASYAI_AUDIO_COREAUDIO)
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->ready.clear();
#elif defined(EASYAI_AUDIO_ALSA)
    if (p_->pcm) { snd_pcm_drop(p_->pcm); snd_pcm_prepare(p_->pcm); }
#endif
}

bool Capture::read(std::vector<std::int16_t> & period) {
    if (!p_->opened) { p_->error = "capture not open"; return false; }
#if defined(EASYAI_AUDIO_COREAUDIO)
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->cv.wait(lk, [&] { return !p_->ready.empty() || !p_->opened; });
    if (p_->ready.empty()) { p_->error = "capture closed"; return false; }
    period = std::move(p_->ready.front());
    p_->ready.pop_front();
    return true;
#elif defined(EASYAI_AUDIO_ALSA)
    period.resize((std::size_t) p_->period);
    std::size_t got = 0;
    while (got < period.size()) {
        snd_pcm_sframes_t n = snd_pcm_readi(p_->pcm, period.data() + got, period.size() - got);
        if (n == -EPIPE) {            // overrun: the consumer was busy (speaking); recover and continue
            snd_pcm_prepare(p_->pcm);
            continue;
        }
        if (n == -EAGAIN) continue;
        if (n < 0) {
            n = snd_pcm_recover(p_->pcm, (int) n, 1);
            if (n < 0) { p_->error = std::string("snd_pcm_readi: ") + snd_strerror((int) n); return false; }
            continue;
        }
        got += (std::size_t) n;
    }
    return true;
#else
    (void) period;
    p_->error = "no audio backend";
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

Playback::Playback()  = default;
Playback::~Playback() = default;

#if defined(EASYAI_AUDIO_COREAUDIO)
namespace {
struct PlayState {
    std::mutex              mu;
    std::condition_variable cv;
    int                     outstanding = 0;
};
void on_output_done(void * user, AudioQueueRef q, AudioQueueBufferRef buf) {
    auto * st = static_cast<PlayState *>(user);
    AudioQueueFreeBuffer(q, buf);
    std::lock_guard<std::mutex> lk(st->mu);
    --st->outstanding;
    st->cv.notify_all();
}
}  // namespace
#endif

bool Playback::play(const std::string & device, const std::int16_t * pcm, std::size_t n, int rate_hz, std::string & err) {
    if (!pcm || n == 0) return true;
#if defined(EASYAI_AUDIO_COREAUDIO)
    (void) device;
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate       = rate_hz;
    fmt.mFormatID         = kAudioFormatLinearPCM;
    fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mBitsPerChannel   = 16;
    fmt.mChannelsPerFrame = 1;
    fmt.mBytesPerFrame    = 2;
    fmt.mFramesPerPacket  = 1;
    fmt.mBytesPerPacket   = 2;
    PlayState st;
    AudioQueueRef q = nullptr;
    if (AudioQueueNewOutput(&fmt, on_output_done, &st, nullptr, nullptr, 0, &q) != noErr) { err = "AudioQueueNewOutput failed"; return false; }
    const std::size_t chunk = (std::size_t) rate_hz * kPlayChunkMs / kMsPerSecond;
    for (std::size_t off = 0; off < n; off += chunk) {
        const std::size_t len = std::min(chunk, n - off);
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(q, (UInt32) (len * sizeof(std::int16_t)), &buf) != noErr) { err = "AudioQueueAllocateBuffer failed"; AudioQueueDispose(q, true); return false; }
        std::memcpy(buf->mAudioData, pcm + off, len * sizeof(std::int16_t));
        buf->mAudioDataByteSize = (UInt32) (len * sizeof(std::int16_t));
        { std::lock_guard<std::mutex> lk(st.mu); ++st.outstanding; }
        AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    }
    if (AudioQueueStart(q, nullptr) != noErr) { err = "AudioQueueStart failed"; AudioQueueDispose(q, true); return false; }
    {
        std::unique_lock<std::mutex> lk(st.mu);
        st.cv.wait(lk, [&] { return st.outstanding == 0; });
    }
    AudioQueueStop(q, false);     // false: let the last buffer finish
    AudioQueueDispose(q, false);
    return true;
#elif defined(EASYAI_AUDIO_ALSA)
    snd_pcm_t * pcm_dev = nullptr;
    int rc = snd_pcm_open(&pcm_dev, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) { err = std::string("snd_pcm_open(") + device + "): " + snd_strerror(rc); return false; }
    const unsigned latency_us = (unsigned) kPlayChunkMs * 2 * kMsPerSecond;
    rc = snd_pcm_set_params(pcm_dev, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, (unsigned) rate_hz, 1, latency_us);
    if (rc < 0) { err = std::string("ALSA playback params: ") + snd_strerror(rc); snd_pcm_close(pcm_dev); return false; }
    std::size_t off = 0;
    while (off < n) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm_dev, pcm + off, n - off);
        if (w < 0) {
            w = snd_pcm_recover(pcm_dev, (int) w, 1);
            if (w < 0) { err = std::string("snd_pcm_writei: ") + snd_strerror((int) w); snd_pcm_close(pcm_dev); return false; }
            continue;
        }
        off += (std::size_t) w;
    }
    snd_pcm_drain(pcm_dev);
    snd_pcm_close(pcm_dev);
    return true;
#else
    (void) device; (void) rate_hz;
    err = "built without an audio backend";
    return false;
#endif
}

}  // namespace audio
}  // namespace easyai
