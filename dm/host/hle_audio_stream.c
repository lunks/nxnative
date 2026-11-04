// nn::audio output streams and virtual audio devices (SDK 20 / 21, the Switch 2's audio): the game writes interleaved PCM frames into a stream and the system mixes it into a virtual
// output device. On the console the SDK reaches the devices through auddmg:u (devmgr) and each stream is a ring shared with the system mixer (sysmix); the host serves neither, so the
// SDK's own calls aborted at devmgr::Initialize (Star Fox: Result 0xa99 under GetDefaultVirtualOutputDevice). Here the host answers the devices and keeps each stream's ring itself; the ring is
// played through hle_audio.c's output (real output / silent sink / background take-over / watchdog, DM_AUDIO, DM_AUDIO_MUTE, DM_AUDIO_DUMP), so a stream is paced like an AudioOut.
// The model follows the SDK's client code (SDK 21.4.4, offsets in Star Fox's sdk; OutputStreamImpl at sdk+0x221b70..0x222b20, sysmix::* at sdk+0x2aaa80..0x2ae9d0):
// - OutputStreamType: +0 the OutputStreamImpl pointer (the type's own +8 once open, 0 after Close), the Impl at +8. GetOutputStreamState answers 0 for a type never opened (sdk+0x221360);
//   Wwise asks that of its second stream when it opened only one (main+0xbce927c). States: 0 not open, 1 stopped (Open, Stop), 2 started (Start; sdk+0x221cb8)
// - the ring: capacity frames (OpenOutputStream's first int, sdk+0x2225c0); filled = written - played, free = capacity - filled (sysmix keeps capacity + 1 slots, sdk+0x2ac37c / 0x2ac8f4).
//   WriteOutputStream copies min(frames, free) frames, NULL data writes silence, returns the frames written and counts the rest as refused (sdk+0x2ae31c / 0x2ae360 / 0x2ae3c8);
//   a negative count is the SDK's abort (sdk+0x222944). PurgeOutputStream drops every buffered frame (count 0 = all, sdk+0x2ace58). Stop keeps the ring; Close also destroys the data event.
//   The console's ring is the game's work buffer: the reader's header at +0x40 (its slot index at +0x44, the mixer's), the writer's at +0x1000 (index at +0x1004), the frames from +0x1040
//   (sdk+0x2ae09c..0x2ae0d0). No game seen reads it directly, so the host's ring is its own (PCM16, in host memory)
// - formats (StreamFormat {int sample format, int channels, int rate}): 2 PcmInt16, 3 PcmInt24, 4 PcmInt32, 5 PcmFloat (4 bytes each but Int16; sdk+0xc03cd8); 1, 2, 6 or 8 channels and
//   8 / 16 / 22.05 / 32 / 44.1 / 48 kHz, anything else is the SDK's abort (sdk+0x222764). The host converts to PCM16 as it writes and plays 48 kHz only, as AudioOut
// - the data event (the overloads with an os::SystemEvent*): attached auto-clear with notification mode 2 and the second int as threshold (sdk+0x2224f0, 0x2ac2cc); a negative threshold is
//   0xc8899 (sdk+0x2aad98). The server's rule is not in the SDK (no SDK code of its own opens a stream with one). The host signals it while at most threshold frames are buffered, checked
//   at open, after each write and purge (a game may wait on it before its first write and before Start) and after each consumer pass. Wwise (main+0xbce90cc) passes capacity - one
//   period (2048 - 512), waits on the event with a 40 ms timeout (its "AK Audio Sink Wakeup Thread", main+0xbce8df4) and then writes whole periods while one is free (main+0xbce9914):
//   so the event comes once a period is free. Read as "at least threshold frames free" instead, it would come with one period left and the timeout would drive Wwise
// - GetOutputStreamStatistics(bool reset) returns {frames written, frames refused, frames played without data} since the last reset (sdk+0x2ac788..0x2ac7d8, copied out by sdk+0x222a2c);
//   Wwise marks its sink starved when the third is above 0 (main+0xbce9ba0)
// - SetOutputStreamGain(gain, TimeSpan): the host ramps linearly to gain over that time as it plays; GetOutputStreamGain reads the gain last set (1.0 at open)
// - StartOutputStreams: 1 to 4 streams, each open and stopped and all on one device, or 0x499 and none started (sdk+0x2217e0); the console engages them together, the host starts them in turn
// - devices: one virtual output device, "MainAudioOut" (the name the SDK's own renderer asks devmgr for, sdk+0x23814c), stereo, 48 kHz; "default" is the first listed name (sdk+0x2214f0).
//   Its properties: the name, the minimum stream duration as a TimeSpan at +0x100 (Open refuses a capacity below it with 0x899, sdk+0x22242c), the device profile's word +0x108 at +0x108
//   (sdk+0x221644: its channel count by a behavioural spec of another emulator, where a game failed when it read 0).
//   An unknown device is 0x299 (devmgr's not-found as the SDK maps it, sdk+0x2225fc). Input devices: none, as on a console without a microphone; opening an input stream fails with 0x299
#include "hle.h"
#include "hle_audio.h"
#include <stdatomic.h>

enum { OS_RATE = 48000, OS_NAME = 256, OS_MAXDEV = 8 };
enum { ST_CLOSED = 0, ST_STOPPED = 1, ST_STARTED = 2 };
enum { FMT_INT16 = 2, FMT_INT24 = 3, FMT_INT32 = 4, FMT_FLOAT = 5 };
#define OS_LATENCY_NS 5000000ll   // the host's device: about 5 ms per callback (hle_audio.c au_buffer), the silent sink 5 ms per pull
#define R_STATE 0x499u            // StartOutputStream(s): a stream not open and stopped, or not attachable (sdk+0x221cd4, 0x221b3c)
#define R_NO_DEVICE 0x299u        // no such virtual device
#define R_CAPACITY 0x899u         // capacity below the device's minimum duration
#define R_BUFFER 0xa99u           // work buffer too small (sysmix::OpenStreamImpl, sdk+0x2ac5ac)
static const char k_out_name[] = "MainAudioOut";

typedef struct { int32_t fmt, ch, rate; } stream_format;
typedef struct { int64_t written, refused, underrun; } stream_stats;   // OutputStreamStatistics
typedef struct {
    pthread_mutex_t m;                     // the game's side: Write, Purge, statistics
    void *g, *ev;                          // the guest OutputStreamType; its data event (NULL: opened without one)
    stream_format f;                       // as opened; GetOutputStreamFormat points here
    int cap, thr, out_id, nlog;            // capacity frames, data event threshold (frames buffered), hle_audio.c output number, periodic lines logged
    uint32_t flags;
    char dev[OS_NAME];
    _Atomic int state;
    int16_t *ring;                         // cap frames × channels, PCM16
    _Atomic uint64_t wr, rd;               // frames written / played since open: wr - rd = buffered, at most cap
    int64_t st_written, st_refused;        // statistics since the last reset (under m)
    _Atomic int64_t st_underrun;           // the consumer's
    uint64_t tot_written, tot_refused; _Atomic uint64_t tot_underrun;   // since open, for the log
    uint64_t t_first, t_next;              // first write; next periodic log line
    _Atomic uint32_t gain_set;             // float bits: the gain last set (GetOutputStreamGain)
    _Atomic uint64_t gain_req; _Atomic uint32_t gain_seq;   // the last SetOutputStreamGain: float bits | ramp frames << 32, then its sequence number
    float g_cur, g_step, g_target; uint32_t g_left, g_seen; // the consumer's ramp
} ostream;

static ostream *os_find(const void *g) { return g ? objmap_get(g) : NULL; }
static ostream *os_of(const void *g, const char *what) {
    ostream *s = os_find(g);
    if (!s) die("%s on OutputStreamType %p, which is not open (the SDK reads its null implementation pointer)", what, g);
    return s;
}
static int bytes_per_sample(int fmt) { return fmt == 2 ? 2 : 4; }
static float f_of(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
static uint32_t bits_of(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }

// The consumer (hle_audio.c pull: the AudioUnit callback or the silent sink, one at a time) takes frames from the ring and applies the gain ramp; it shares only atomics with the game's side.
// A Purge moves rd forward under it: the compare-exchange then fails and the purged frames stay purged (what this pass copied out was already played)
static void os_pull(void *p, int16_t *dst, uint32_t frames, dm_pcm_pull *r) {
    ostream *s = p; const int ch = s->f.ch;
    uint32_t seq = atomic_load_explicit(&s->gain_seq, memory_order_acquire);
    if (seq != s->g_seen) {
        uint64_t q = atomic_load_explicit(&s->gain_req, memory_order_relaxed);
        float target = f_of((uint32_t)q); uint32_t n = (uint32_t)(q >> 32);
        s->g_seen = seq; s->g_target = target; s->g_left = n; s->g_step = n ? (target - s->g_cur) / (float)n : 0.f; if (!n) s->g_cur = target;
    }
    uint64_t rd = atomic_load_explicit(&s->rd, memory_order_acquire), wr = atomic_load_explicit(&s->wr, memory_order_acquire);
    uint64_t have = wr - rd; uint32_t n = have < frames ? (uint32_t)have : frames;
    for (uint32_t i = 0; i < n; i++) {
        const int16_t *src = s->ring + ((rd + i) % (uint64_t)s->cap) * ch;
        int16_t *o = dst + (size_t)i * ch;
        if (s->g_left) { s->g_cur += s->g_step; if (!--s->g_left) s->g_cur = s->g_target; }
        if (s->g_cur == 1.f) { memcpy(o, src, sizeof *o * ch); continue; }
        for (int c = 0; c < ch; c++) { float x = src[c] * s->g_cur; o[c] = (int16_t)(x > 32767.f ? 32767.f : x < -32768.f ? -32768.f : x); }
    }
    if (n < frames) memset(dst + (size_t)n * ch, 0, sizeof *dst * ch * (frames - n));
    if (n) atomic_compare_exchange_strong_explicit(&s->rd, &rd, rd + n, memory_order_acq_rel, memory_order_acquire);
    uint32_t miss = frames - n;
    if (miss) { atomic_fetch_add_explicit(&s->st_underrun, miss, memory_order_relaxed); atomic_fetch_add_explicit(&s->tot_underrun, miss, memory_order_relaxed); }
    uint64_t left = atomic_load_explicit(&s->wr, memory_order_acquire) - atomic_load_explicit(&s->rd, memory_order_acquire);
    r->missing = miss; r->filled = (uint32_t)left; r->signal = s->ev && left <= (uint64_t)s->thr;
}

// The data event's level condition, checked on the game's side (open, write, purge): the consumer's passes check it in os_pull
static void os_signal_if_due(ostream *s) {
    if (s->ev && atomic_load_explicit(&s->wr, memory_order_acquire) - atomic_load_explicit(&s->rd, memory_order_acquire) <= (uint64_t)s->thr) hle_signal_system_event(s->ev);
}
static int os_valid_rate(int r) { return r == 8000 || r == 16000 || r == 22050 || r == 32000 || r == 44100 || r == 48000; }
static int os_device_is_ours(const char *name) { return name && !strncmp(name, k_out_name, OS_NAME); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static Result os_open(void *g, void *ev, void *buf, uint64_t size, const char *name, const stream_format *fmt, int cap, int thr, uint32_t flags, void *lr) {
    stream_format f = *fmt;
    // In the SDK's order: the device and its minimum duration (devmgr), the format's name (aborts), sysmix's work buffer checks, the data event
    if (!os_device_is_ours(name)) { LOG("audio stream: OpenOutputStream on device \"%.64s\": the host has only %s (0x299)", name ? name : "", k_out_name); return R_NO_DEVICE; }
    if (cap < (OS_LATENCY_NS * f.rate + 999999999) / 1000000000) return R_CAPACITY;
    if (f.ch < 1 || f.ch > 8 || !((0xa3u >> (f.ch - 1)) & 1) || !os_valid_rate(f.rate) || f.fmt < FMT_INT16 || f.fmt > FMT_FLOAT)
        die("OpenOutputStream: format %d, %d channels, %d Hz: the SDK aborts on it (UnexpectedDefault, sdk+0x222864)", f.fmt, f.ch, f.rate);
    // sysmix::OpenStreamImpl's checks of the work buffer (sdk+0x2ac370..0x2ac4d0); the host keeps its ring elsewhere, the console keeps it in this buffer
    uint64_t data = align_up((uint64_t)bytes_per_sample(f.fmt) * (uint64_t)(cap + 1) * (uint64_t)f.ch, 64);
    uint64_t slots = (uint64_t)(cap + 1 < 16 ? cap + 1 : 16);
    if (size < ((align_up(slots * 8 + 64, 64) + 0x1fff + data) & ~0xfffull)) return R_BUFFER;
    if (!buf) return 0xc8899u;
    if (size < ((data + 0x203f) & ~0xfffull)) return 0xc8e99u;
    if ((uintptr_t)buf & 0xfff) return 0xc8a99u;
    if (size & 0xfff) return 0xc8e99u;
    if (ev && thr < 0) return 0xc8899u;   // sysmix::AcquireStreamDataEvent (sdk+0x2aad98)
    if (f.rate != OS_RATE) die("OpenOutputStream at %d Hz: the host plays 48 kHz only", f.rate);
    if (f.fmt == FMT_INT24) die("OpenOutputStream in PcmInt24: the host does not convert it yet (its 24 bits' place in the 4-byte sample is not known)");
    if (f.ch == 8) LOG("[RED] audio stream: 8 channels go to the device as is (Core Audio keeps the front pair); the host downmixes only 6");
    if (flags) LOG("[RED] audio stream: OutputStreamFlag %#x (sysmix creation options, sdk+0x222454) not modelled: played as without it", flags);
    if (os_find(g)) die("OpenOutputStream on OutputStreamType %p, which is already open", g);
    ostream *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    s->g = g; s->ev = ev; s->f = f; s->cap = cap; s->thr = thr; s->flags = flags;
    snprintf(s->dev, sizeof s->dev, "%s", name);
    s->ring = calloc((size_t)cap * f.ch, sizeof *s->ring);
    atomic_init(&s->state, ST_STOPPED); atomic_init(&s->wr, 0); atomic_init(&s->rd, 0); atomic_init(&s->st_underrun, 0); atomic_init(&s->tot_underrun, 0);
    atomic_init(&s->gain_set, bits_of(1.f)); atomic_init(&s->gain_req, bits_of(1.f)); atomic_init(&s->gain_seq, 0); s->g_cur = 1.f;
    s->out_id = dm_pcmout_open_source(s, f.ch, ev, name, os_pull, s, lr);
    static const char *const fn[] = {"?", "?", "PcmInt16", "PcmInt24", "PcmInt32", "PcmFloat"};
    char th[48] = ""; if (ev) snprintf(th, sizeof th, ", data event at <= %d frames buffered", thr);
    LOG("audio #%d stream: %s, %d channels, %d Hz, %d frames (%.1f ms)%s, work buffer %#llx bytes", s->out_id, fn[f.fmt], f.ch, f.rate, cap, cap * 1000.0 / f.rate, th, (unsigned long long)size);
    objmap_put(g, s);
    *(uint64_t *)g = (uint64_t)(uintptr_t)g + 8;   // the Impl pointer as the SDK's open leaves it (sdk+0x221124)
    os_signal_if_due(s);   // the empty ring: due at once
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "opens a sysmix stream on an auddmg:u virtual device; a host ring played by the host's audio output", Result, h_os_OpenEvF,
          void *g, void *ev, void *buf, uint64_t size, const char *name, const stream_format *f, int cap, int thr, uint32_t flags) {
    return os_open(g, ev, buf, size, name, f, cap, thr, flags, __builtin_return_address(0));   // flags: the ninth argument, on the stack in both ABIs
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii",
          "opens a sysmix stream (no flags); a host ring", Result, h_os_OpenEv, void *g, void *ev, void *buf, uint64_t size, const char *name, const stream_format *f, int cap, int thr) {
    return os_open(g, ev, buf, size, name, f, cap, thr, 0, __builtin_return_address(0));
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "opens a sysmix stream without a data event; a host ring", Result, h_os_OpenF, void *g, void *buf, uint64_t size, const char *name, const stream_format *f, int cap, uint32_t flags) {
    return os_open(g, NULL, buf, size, name, f, cap, 0, flags, __builtin_return_address(0));
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi",
          "opens a sysmix stream without a data event or flags; a host ring", Result, h_os_Open, void *g, void *buf, uint64_t size, const char *name, const stream_format *f, int cap) {
    return os_open(g, NULL, buf, size, name, f, cap, 0, 0, __builtin_return_address(0));
}

// Close: the output's teardown (Core Audio, which needs a big stack) on a host thread, then the data event destroyed with the SDK's own call on the game's thread, as ReleaseStreamDataEvent does
// (sdk+0x2ac32c): the game may attach it again at its next open. hle_audio.c has joined the thread that signals it by then
typedef struct { ostream *s; void *lr; } os_close_arg;
static void *os_close_th(void *p) { os_close_arg *a = p; dm_pcmout_close_at(a->s, a->lr); return NULL; }
static void os_close(ostream *s, void *lr) {
    pthread_t t; os_close_arg a = {s, lr};
    if (pthread_create(&t, NULL, os_close_th, &a) || dm_host_thread_join(t, NULL)) die("CloseOutputStream: cannot start its thread");
    LOG("audio #%d stream closed: %llu frames written, %llu played, %llu without data, %llu refused (ring full)", s->out_id, (unsigned long long)s->tot_written,
        (unsigned long long)atomic_load(&s->rd), (unsigned long long)atomic_load(&s->tot_underrun), (unsigned long long)s->tot_refused);
    if (s->ev) {
        static void (*destroy)(void *); if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
        destroy(s->ev);
    }
    objmap_del(s->g); *(uint64_t *)s->g = 0;   // the public CloseOutputStream clears the Impl pointer (sdk+0x2212b0)
    pthread_mutex_destroy(&s->m); free(s->ring); free(s);
}
HLE_FORCE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", "closes the sysmix stream; stops and frees the host output", void, h_os_Close, void *g) {
    ostream *s = os_find(g); if (s) os_close(s, __builtin_return_address(0));   // a type never opened: nothing, as the SDK with its null Impl pointer
}
static void os_start(ostream *s, void *lr) { dm_pcmout_start_at(s, lr); atomic_store(&s->state, ST_STARTED); }
HLE_FORCE_BIG("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", "attaches the stream to its virtual device (devmgr); starts the host output", Result, h_os_Start, void *g) {
    os_start(os_of(g, "StartOutputStream"), dm_bigstack_caller(__builtin_return_address(0))); return RES_OK;
}
HLE_FORCE_BIG("_ZN2nn5audio18StartOutputStreamsEPKPNS0_16OutputStreamTypeEi", "attaches several streams at once; started in turn on the host", Result, h_os_StartN, void *const *gs, int n) {
    void *lr = dm_bigstack_caller(__builtin_return_address(0)); ostream *s[4];
    if (!gs || n < 1 || n > 4) return R_STATE;
    for (int i = 0; i < n; i++) {
        s[i] = os_find(gs[i]);
        if (!s[i] || atomic_load(&s[i]->state) != ST_STOPPED || strncmp(s[i]->dev, s[0]->dev, OS_NAME)) return R_STATE;
    }
    for (int i = 0; i < n; i++) os_start(s[i], lr);
    return RES_OK;
}
HLE_FORCE_BIG("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", "detaches the stream from its device; stops the host output, the ring keeps its frames", void, h_os_Stop, void *g) {
    ostream *s = os_of(g, "StopOutputStream");
    dm_pcmout_stop_at(s, dm_bigstack_caller(__builtin_return_address(0))); atomic_store(&s->state, ST_STOPPED);
}
HLE_FORCE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", "stream state: 0 not open, 1 stopped, 2 started", int, h_os_State, const void *g) {
    ostream *s = os_find(g); return s ? atomic_load(&s->state) : ST_CLOSED;
}
static int os_filled(ostream *s) { return (int)(atomic_load_explicit(&s->wr, memory_order_acquire) - atomic_load_explicit(&s->rd, memory_order_acquire)); }
HLE_FORCE("_ZN2nn5audio33GetOutputStreamCapacityFrameCountEPKNS0_16OutputStreamTypeE", "ring capacity in frames", int, h_os_Cap, const void *g) { return os_of(g, "GetOutputStreamCapacityFrameCount")->cap; }
HLE_FORCE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", "frames the ring can take", int, h_os_Free, const void *g) {
    ostream *s = os_of(g, "GetOutputStreamFreeFrameCount"); return s->cap - os_filled(s);
}
HLE_FORCE("_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", "frames buffered, not yet played", int, h_os_Filled, const void *g) {
    return os_filled(os_of(g, "GetOutputStreamFilledFrameCount"));
}
static void os_convert(const ostream *s, int16_t *dst, const uint8_t *src, uint32_t frames) {
    uint32_t n = frames * (uint32_t)s->f.ch;
    if (!src) { memset(dst, 0, sizeof *dst * n); return; }
    if (s->f.fmt == FMT_INT16) { memcpy(dst, src, sizeof *dst * n); return; }
    for (uint32_t i = 0; i < n; i++) {
        if (s->f.fmt == FMT_INT32) { int32_t v; memcpy(&v, src + 4 * i, 4); dst[i] = (int16_t)(v >> 16); continue; }
        float x; memcpy(&x, src + 4 * i, 4); x *= 32768.f;
        dst[i] = (int16_t)(x >= 32767.f ? 32767.f : x <= -32768.f ? -32768.f : x == x ? x : 0.f);
    }
}
// Write: at most the free frames, from the game's thread (Wwise's audio thread); the ring's wr is published after the copy, so the consumer reads only frames already converted
static int os_write(ostream *s, const void *data, int frames) {
    if (frames < 0) die("WriteOutputStream with %d frames: sysmix refuses a negative count and the SDK aborts (sdk+0x222944)", frames);
    pthread_mutex_lock(&s->m);
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed), rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    uint32_t space = (uint32_t)(s->cap - (int)(wr - rd)), n = (uint32_t)frames < space ? (uint32_t)frames : space;
    const uint8_t *src = data; size_t step = (size_t)bytes_per_sample(s->f.fmt) * s->f.ch;
    for (uint32_t done = 0; done < n;) {
        uint32_t at = (uint32_t)((wr + done) % (uint64_t)s->cap), k = n - done;
        if (k > (uint32_t)s->cap - at) k = (uint32_t)s->cap - at;
        os_convert(s, s->ring + (size_t)at * s->f.ch, src ? src + done * step : NULL, k);
        dm_audio_dump(s->ring + (size_t)at * s->f.ch, sizeof *s->ring * k * s->f.ch);
        done += k;
    }
    atomic_store_explicit(&s->wr, wr + n, memory_order_release);
    s->st_written += n; s->st_refused += frames - (int)n; s->tot_written += n; s->tot_refused += (uint32_t)frames - n;
    // A line 0.1 s after the first write (a bring-up that dies early still shows the stream ran), 2 s, then every 10 s for a minute, then every minute; at most 16 per stream
    // (the device-callback lines are hle_audio.c's, with real output only)
    uint64_t t = now_ns();
    if (!s->t_first) { s->t_first = t; s->t_next = t + 100000000ull; }
    if (t >= s->t_next && s->nlog < 16) {
        uint64_t since = t - s->t_first;
        s->nlog++; s->t_next = since < 2000000000ull ? s->t_first + 2000000000ull : t + (since < 60000000000ull ? 10000000000ull : 60000000000ull);
        LOG("audio #%d stream after %.1f s: %llu frames written, %llu played, %llu without data, %llu refused (ring full), %d buffered%s", s->out_id, (t - s->t_first) / 1e9,
            (unsigned long long)s->tot_written, (unsigned long long)atomic_load(&s->rd), (unsigned long long)atomic_load(&s->tot_underrun), (unsigned long long)s->tot_refused,
            (int)(wr + n - atomic_load(&s->rd)), s->nlog == 16 ? "; not logged from here on" : "");
    }
    pthread_mutex_unlock(&s->m);
    os_signal_if_due(s);
    return (int)n;
}
HLE_FORCE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", "copies frames into the stream's ring; returns the frames written", int, h_os_Write, void *g, const void *data, int frames) {
    return os_write(os_of(g, "WriteOutputStream"), data, frames);
}
static void os_purge(ostream *s) {
    pthread_mutex_lock(&s->m);
    uint64_t wr = atomic_load_explicit(&s->wr, memory_order_relaxed), rd = atomic_load_explicit(&s->rd, memory_order_acquire);
    while (rd != wr && !atomic_compare_exchange_weak_explicit(&s->rd, &rd, wr, memory_order_acq_rel, memory_order_acquire)) {}
    pthread_mutex_unlock(&s->m);
    os_signal_if_due(s);
}
HLE_FORCE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", "drops every frame buffered", void, h_os_Purge, void *g) { os_purge(os_of(g, "PurgeOutputStream")); }
static stream_stats os_stats(ostream *s, int reset) {
    pthread_mutex_lock(&s->m);
    stream_stats r = {s->st_written, s->st_refused, reset ? atomic_exchange(&s->st_underrun, 0) : atomic_load(&s->st_underrun)};
    if (reset) s->st_written = s->st_refused = 0;
    pthread_mutex_unlock(&s->m);
    return r;
}
HLE_FORCE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", "frames written, refused and played without data since the last reset", stream_stats, h_os_Stats, void *g, unsigned reset) {
    return os_stats(os_of(g, "GetOutputStreamStatistics"), reset & 1);   // bool: Switch code does not extend it
}
static void os_set_gain(ostream *s, float gain, int64_t ramp_ns) {
    if (!(gain >= 0.f)) gain = 0.f;
    uint64_t frames = ramp_ns > 0 ? (uint64_t)ramp_ns * OS_RATE / 1000000000u : 0; if (frames > 0xffffffffu) frames = 0xffffffffu;
    atomic_store(&s->gain_set, bits_of(gain));
    atomic_store_explicit(&s->gain_req, bits_of(gain) | frames << 32, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->gain_seq, 1, memory_order_release);
}
HLE_FORCE("_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", "stream gain, ramped over the TimeSpan as the host plays", void, h_os_SetGain, void *g, float gain, int64_t ramp_ns) {
    os_set_gain(os_of(g, "SetOutputStreamGain"), gain, ramp_ns);
}
HLE_FORCE("_ZN2nn5audio19GetOutputStreamGainEPKNS0_16OutputStreamTypeE", "the stream gain last set", float, h_os_GetGain, const void *g) { return f_of(atomic_load(&os_of(g, "GetOutputStreamGain")->gain_set)); }
HLE_FORCE("_ZN2nn5audio21GetOutputStreamFormatEPKNS0_16OutputStreamTypeE", "the stream's format as opened", const stream_format *, h_os_Format, const void *g) { return &os_of(g, "GetOutputStreamFormat")->f; }

// Virtual devices (devmgr over auddmg:u on the console)
static int os_list(char *names, int n, int have) {   // as detail::ListVirtual*Devices: at most 8 entries, the ones past the devices emptied (sdk+0x2213e8..0x2214a8)
    if (n <= 0) return 0;
    int k = n < OS_MAXDEV ? n : OS_MAXDEV;
    for (int i = 0; i < k; i++) names[i * OS_NAME] = 0;
    if (have) snprintf(names, OS_NAME, "%s", k_out_name);
    return have ? 1 : 0;
}
HLE_FORCE("_ZN2nn5audio24ListVirtualOutputDevicesEPNS0_15AudioDeviceNameEi", "virtual output devices (auddmg:u); the host has one", int, h_vd_ListOut, char *names, int n) { return os_list(names, n, 1); }
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "default virtual output device (auddmg:u): the host's one", void, h_vd_DefOut, char *name) { snprintf(name, OS_NAME, "%s", k_out_name); }
typedef struct { char name[OS_NAME]; int64_t min_ns; uint32_t channels; uint32_t pad; } vdev_props;   // VirtualDeviceProperties (sdk+0x221628..0x221648)
HLE_FORCE("_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "virtual output device properties (auddmg:u)", Result, h_vd_PropOut,
          vdev_props *p, const char *name) {
    if (!os_device_is_ours(name)) return R_NO_DEVICE;
    memcpy(p->name, name, OS_NAME); p->min_ns = OS_LATENCY_NS;
    p->channels = 2;   // VirtualDeviceProfile+0x108: the device's channel count (see the top of the file)
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", "virtual input devices (auddmg:u); the host has no microphone: none", int, h_vd_ListIn, char *names, int n) { return os_list(names, n, 0); }
HLE_FORCE("_ZN2nn5audio31GetVirtualInputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "virtual input device properties; no input device on the host", Result, h_vd_PropIn,
          vdev_props *p, const char *name) { (void)p; (void)name; return R_NO_DEVICE; }

// Input streams: no device is listed, so a game that checks the list (Star Fox, main+0xb66d648) never opens one; an open fails as for a device that is not there. The calls that need an open
// stream answer without touching the game's object, in case a game ignores the open's result (the SDK would read its null Impl pointer)
static Result is_open(const char *name, void *lr) {
    static _Atomic int once; char sb[64]; module_t *m = lr ? module_of(lr) : NULL;
    if (m) snprintf(sb, sizeof sb, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(sb, sizeof sb, "host");
    if (!atomic_exchange(&once, 1)) LOG("audio input stream %.64s: the host has no microphone, the open fails (0x299; call site %s)", name ? name : "", sb);
    return R_NO_DEVICE;
}
static void is_unopened(const char *what) { static _Atomic int once; if (!atomic_exchange(&once, 1)) LOG("[RED] audio: %s on an input stream that never opened (no microphone on the host)", what); }
HLE_FORCE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", "opens a sysmix input stream; no microphone on the host: fails",
          Result, h_is_Open, void *g, void *buf, uint64_t size, const char *name, const void *f, int cap) {
    (void)g; (void)buf; (void)size; (void)f; (void)cap; return is_open(name, __builtin_return_address(0));
}
HLE_FORCE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii", "opens a sysmix input stream with a data event; no microphone on the host: fails",
          Result, h_is_OpenEv, void *g, void *ev, void *buf, uint64_t size, const char *name, const void *f, int cap, int thr) {
    (void)g; (void)ev; (void)buf; (void)size; (void)f; (void)cap; (void)thr; return is_open(name, __builtin_return_address(0));
}
HLE_FORCE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", "never opened on the host: fails", Result, h_is_Start, void *g) { (void)g; is_unopened("StartInputStream"); return R_STATE; }
HLE_FORCE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", "never opened on the host", void, h_is_Stop, void *g) { (void)g; is_unopened("StopInputStream"); }
HLE_FORCE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", "never opened on the host: nothing to close (the SDK's own skips a null Impl pointer)", void, h_is_Close, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", "never opened on the host: no frames", int, h_is_Read, void *g, void *buf, int n) { (void)g; (void)buf; (void)n; is_unopened("ReadInputStream"); return 0; }
HLE_FORCE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", "never opened on the host: none buffered", int, h_is_Filled, const void *g) { (void)g; is_unopened("GetInputStreamFilledFrameCount"); return 0; }
HLE_FORCE("_ZN2nn5audio20GetInputStreamFormatEPKNS0_15InputStreamTypeE", "never opened on the host: an empty format", const stream_format *, h_is_Format, const void *g) {
    static const stream_format none; (void)g; is_unopened("GetInputStreamFormat"); return &none;
}
