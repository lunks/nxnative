// SDK 20+ audio output streams and virtual devices as the host serves them (hle_audio_stream.c on hle_audio.c's output), without a guest: the device list and properties, the open's
// Results, the ring's free / filled counts, Write's partial writes and conversion to PCM16, the consumer's underruns, the data event's threshold, statistics with reset, the gain ramp,
// Purge, StartOutputStreams' checks, and then the silent sink playing a started stream in real time (frames consumed, the event signaled, nothing played while stopped, Close).
#include "../hle_audio.c"
#include "../hle_audio_stream.c"
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
#include <math.h>
#include <time.h>

uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[32];
static pthread_mutex_t om_m = PTHREAD_MUTEX_INITIALIZER;
void *objmap_get(const void *k) { void *v = NULL; pthread_mutex_lock(&om_m); for (int i = 0; i < 32; i++) if (om[i].k == k) { v = om[i].v; break; } pthread_mutex_unlock(&om_m); return v; }
void objmap_put(const void *k, void *v) { pthread_mutex_lock(&om_m); for (int i = 0; i < 32; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; break; } pthread_mutex_unlock(&om_m); }
void *objmap_del(const void *k) { void *v = NULL; pthread_mutex_lock(&om_m); for (int i = 0; i < 32; i++) if (om[i].k == k) { om[i].k = NULL; v = om[i].v; break; } pthread_mutex_unlock(&om_m); return v; }
static _Atomic int g_sig; static void *g_bound; static int g_bound_auto = -1; static void *g_destroyed;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add(&g_sig, 1); }
void hle_bind_system_event(void *g, int autoclear) { g_bound = g; g_bound_auto = autoclear; }
module_t *module_of(const void *p) { (void)p; return NULL; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }   // no guest address space: the big-stack take-overs run on the calling stack
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static void destroy_stub(void *ev) { g_destroyed = ev; }
void *guest_sym(const char *name) { return strcmp(name, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? NULL : (void *)destroy_stub; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint64_t impl; uint8_t rest[0x878]; } os_type;   // OutputStreamType: the Impl pointer, then the Impl (Wwise's spacing, main+0xbce90b4)
static _Alignas(4096) uint8_t work[4][0x4000];
static uint8_t evbuf[2][0x30];   // two guest SystemEvents
static void ms(int n) { usleep((useconds_t)n * 1000); }

int main(void) {
    setenv("DM_AUDIO", "0", 1);   // the silent sink: no sound device needed
    char names[3][256]; memset(names, 0x55, sizeof names);
    // Devices
    CHECK(h_vd_ListOut(&names[0][0], 0) == 0, "an empty list");
    CHECK(h_vd_ListOut(&names[0][0], 3) == 1 && !strcmp(names[0], "MainAudioOut") && !names[1][0] && !names[2][0], "one output device, the rest emptied");
    char def[256]; h_vd_DefOut(def); CHECK(!strcmp(def, "MainAudioOut"), "default output device");
    vdev_props p; memset(&p, 0x55, sizeof p);
    char dn[256] = "MainAudioOut", tv[256] = "AudioTvOutput";   // an AudioDeviceName is 256 bytes, all of them copied
    CHECK(h_vd_PropOut(&p, dn) == 0 && !strcmp(p.name, "MainAudioOut") && p.min_ns == 5000000 && p.channels == 2, "output device properties");
    CHECK(h_vd_PropOut(&p, tv) == 0x299, "an unknown output device is 0x299");
    memset(names, 0x55, sizeof names);
    CHECK(h_vd_ListIn(&names[0][0], 2) == 0 && !names[0][0] && !names[1][0] && names[2][0] == 0x55, "no input device, two entries emptied");
    CHECK(h_vd_PropIn(&p, "VoiceChatIn") == 0x299, "no input device properties");
    stream_format in = {FMT_INT16, 1, 48000}; os_type it = {0};
    CHECK(h_is_Open(&it, work[3], sizeof work[3], "VoiceChatIn", &in, 240) == 0x299 && !it.impl, "an input stream does not open");
    // What Ocarina of Time asks of its two never-opened input streams: state, purge, statistics; and the other calls that would read the null Impl pointer
    memset(&it, 0x55, sizeof it);   // the game's object is not read
    h_is_Purge(&it); stream_stats ist = h_is_Stats(&it, 1);
    CHECK(h_is_State(&it) == 0 && !ist.written && !ist.refused && !ist.underrun, "a never-opened input stream: closed, zero statistics");
    CHECK(h_is_Free(&it) == 0 && h_is_Capacity(&it) == 0 && h_is_GetGain(&it) == 1.f, "and no frames, gain 1");
    h_is_SetGain(&it, 0.5f, 1000000); CHECK(h_is_GetGain(&it) == 1.f, "a gain set on it is not kept");
    char din[256]; memset(din, 0x55, sizeof din); h_vd_DefIn(din);
    CHECK(!din[0] && !h_vd_FmtIn("VoiceChatIn", &in), "no default input device, no input format");

    // Open's Results
    os_type t = {0}, t2 = {0}, never = {0};
    stream_format ff = {FMT_FLOAT, 2, 48000};
    CHECK(h_os_State(&never) == 0, "state of a stream never opened");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x4000, "AudioTvOutput", &ff, 1024, 768, 0) == 0x299, "unknown device");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x4000, "MainAudioOut", &ff, 200, 100, 0) == 0x899, "capacity below the device's 5 ms (240 frames)");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x3000, "MainAudioOut", &ff, 1024, 768, 0) == 0xa99, "work buffer too small (0x4000 needed, what Wwise passes)");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0] + 0x10, 0x4000, "MainAudioOut", &ff, 1024, 768, 0) == 0xc8a99, "unaligned work buffer");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x4800, "MainAudioOut", &ff, 1024, 768, 0) == 0xc8e99, "work buffer size not a page multiple");
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x4000, "MainAudioOut", &ff, 1024, -1, 0) == 0xc8899, "negative threshold");
    CHECK(!t.impl && h_os_State(&t) == 0, "nothing opened by the failures");

    // Wwise's main stream: float stereo, 4 periods of 256 frames, data event at capacity - one period
    CHECK(h_os_OpenEvF(&t, evbuf[0], work[0], 0x4000, "MainAudioOut", &ff, 1024, 768, 0) == 0, "open");
    ostream *s = os_find(&t);
    CHECK(s && t.impl == (uint64_t)(uintptr_t)&t + 8 && h_os_State(&t) == 1, "open: Impl pointer, stopped");
    CHECK(g_bound == evbuf[0] && g_bound_auto == 1, "data event bound auto-clear");
    CHECK(atomic_load(&g_sig) == 1, "the empty ring's data event is due at open (a game may wait on it before its first write and before Start)");
    CHECK(h_os_Cap(&t) == 1024 && h_os_Free(&t) == 1024 && h_os_Filled(&t) == 0, "empty ring");
    const stream_format *gf = h_os_Format(&t); CHECK(gf->fmt == 5 && gf->ch == 2 && gf->rate == 48000, "format as opened");
    CHECK(h_os_GetGain(&t) == 1.f, "gain 1.0 at open");

    // Write: partial writes, conversion
    static float fbuf[2048 * 2];
    for (int i = 0; i < 2048; i++) { fbuf[2 * i] = (float)(i % 100) / 100.f; fbuf[2 * i + 1] = -(float)(i % 100) / 100.f; }
    fbuf[0] = 1.5f; fbuf[1] = -1.0f; fbuf[2] = NAN; fbuf[3] = 0.5f;
    CHECK(h_os_Write(&t, fbuf, 100) == 100 && atomic_load(&g_sig) == 2, "100 buffered: still due after the write");
    CHECK(h_os_Write(&t, fbuf + 200, 900) == 900 && h_os_Free(&t) == 24 && h_os_Filled(&t) == 1000 && atomic_load(&g_sig) == 2, "1000 buffered: not due");
    CHECK(h_os_Write(&t, fbuf + 2000, 100) == 24 && h_os_Free(&t) == 0, "a full ring takes 24 of 100");
    CHECK(h_os_Write(&t, fbuf, 10) == 0, "a full ring takes nothing");
    stream_stats st = h_os_Stats(&t, 0);
    CHECK(st.written == 1024 && st.refused == 86 && st.underrun == 0, "statistics: %lld written, %lld refused, %lld underrun", st.written, st.refused, st.underrun);

    // The consumer: PCM16, the threshold, wraparound, underruns
    static int16_t out[2048 * 2]; dm_pcm_pull r;
    os_pull(s, out, 255, &r);
    CHECK(out[0] == 32767 && out[1] == -32768 && out[2] == 0 && out[3] == 16384, "conversion: saturated, NaN silent, 0.5 = 16384 (%d %d %d %d)", out[0], out[1], out[2], out[3]);
    CHECK(out[2 * 50] == (int16_t)(0.5f * 32768) && out[2 * 50 + 1] == (int16_t)(-0.5f * 32768), "frame 50");
    CHECK(r.missing == 0 && r.filled == 769 && !r.signal, "769 frames left: above the threshold");
    os_pull(s, out, 1, &r);
    CHECK(r.filled == 768 && r.signal, "768 left: the event is due");
    CHECK(h_os_Write(&t, fbuf + 2 * 1000, 256) == 256 && h_os_Filled(&t) == 1024, "a write that wraps around the ring's end");
    os_pull(s, out, 1024, &r);
    CHECK(r.missing == 0 && r.filled == 0 && r.signal, "all played");
    CHECK(out[2 * 744] == (int16_t)((float)(1000 % 100) / 100.f * 32768) && out[2 * 767] == (int16_t)(0.23f * 32768), "frames 1000.. after 744 of the rest (%d %d)", out[2 * 744], out[2 * 767]);
    os_pull(s, out, 100, &r);
    CHECK(r.missing == 100 && out[0] == 0 && out[199] == 0, "an empty ring plays silence, counted");
    st = h_os_Stats(&t, 1);
    CHECK(st.written == 1280 && st.refused == 86 && st.underrun == 100, "statistics before reset: %lld %lld %lld", st.written, st.refused, st.underrun);
    st = h_os_Stats(&t, 1);
    CHECK(!st.written && !st.refused && !st.underrun, "statistics after reset");

    // Gain: immediate, then a 1 ms ramp back up (48 frames)
    for (int i = 0; i < 256; i++) fbuf[2 * i] = fbuf[2 * i + 1] = 0.5f;
    h_os_SetGain(&t, 0.5f, 0); CHECK(h_os_GetGain(&t) == 0.5f, "gain read back");
    h_os_Write(&t, fbuf, 256);
    os_pull(s, out, 8, &r); CHECK(out[0] == 8192 && out[15] == 8192, "gain 0.5 (%d)", out[0]);
    h_os_SetGain(&t, 1.f, 1000000);
    os_pull(s, out, 60, &r);
    int mono = 1; for (int i = 1; i < 48; i++) if (out[2 * i] < out[2 * i - 2]) mono = 0;
    CHECK(mono && out[0] > 8192 && out[0] < 8600 && out[2 * 47] == 16384 && out[2 * 59] == 16384, "ramp: %d .. %d, then %d", out[0], out[2 * 46], out[2 * 59]);

    // Purge, NULL data
    CHECK(h_os_Filled(&t) == 188, "188 left");
    int before = atomic_load(&g_sig);
    h_os_Purge(&t); CHECK(h_os_Filled(&t) == 0 && h_os_Free(&t) == 1024 && atomic_load(&g_sig) == before + 1, "purged, the event due");
    CHECK(h_os_Write(&t, NULL, 10) == 10, "NULL data");
    os_pull(s, out, 10, &r); CHECK(out[0] == 0 && out[19] == 0, "NULL data is silence");

    // A second stream without an event (Int16 and Int32), StartOutputStreams' checks
    stream_format f16 = {FMT_INT16, 2, 48000};
    CHECK(h_os_OpenF(&t2, work[1], 0x2000, "MainAudioOut", &f16, 512, 0) == 0, "open without an event");
    ostream *s2 = os_find(&t2);
    int16_t pcm[8] = {1, -2, 300, -400, 32767, -32768, 7, 8};
    CHECK(h_os_Write(&t2, pcm, 4) == 4, "Int16 write");
    os_pull(s2, out, 4, &r); CHECK(!memcmp(out, pcm, sizeof pcm) && !r.signal, "Int16 as is, no event");
    os_type t3 = {0}; stream_format f32 = {FMT_INT32, 1, 48000}; int32_t p32[2] = {0x12345678, -0x40000000};
    CHECK(h_os_Open(&t3, work[2], 0x2000, "MainAudioOut", &f32, 256) == 0 && h_os_Write(&t3, p32, 2) == 2, "Int32 stream");
    os_pull(os_find(&t3), out, 2, &r); CHECK(out[0] == 0x1234 && out[1] == -0x4000, "Int32 to its top 16 bits");
    void *bad[2] = {&t, &never}, *two[2] = {&t, &t2};
    CHECK(h_os_StartN((void *const *)two, 0) == 0x499 && h_os_StartN((void *const *)bad, 2) == 0x499 && h_os_State(&t) == 1, "StartOutputStreams refuses a stream not open: none started");
    CHECK(h_os_StartN((void *const *)two, 2) == 0 && h_os_State(&t) == 2 && h_os_State(&t2) == 2, "both started");
    CHECK(h_os_StartN((void *const *)two, 2) == 0x499, "started streams are refused");
    h_os_Stop(&t2); h_os_Close(&t2); h_os_Close(&t3);
    CHECK(!t2.impl && h_os_State(&t2) == 0 && !os_find(&t2), "closed, without an event to destroy");
    CHECK(g_destroyed == NULL, "no event destroyed");

    // The silent sink plays the started stream in real time: 50 ms in about 50 ms, then silence counted; the event signaled
    h_os_Stats(&t, 1); atomic_store(&g_sig, 0);
    h_os_SetGain(&t, 1.f, 0);
    CHECK(h_os_Write(&t, fbuf, 1024) == 1024, "ring filled");
    // Deadlines rather than fixed sleeps: the run may share the machine. 1024 frames take 21 ms at 48 kHz; the sink pulls 240 every 5 ms, so emptying takes at least 3 more pulls
    uint64_t t0 = now_ns(), t1 = 0;
    while (h_os_Filled(&t) == 1024 && now_ns() - t0 < 2000000000ull) ms(1);
    t1 = now_ns(); CHECK(h_os_Filled(&t) < 1024, "being played");
    while (h_os_Filled(&t) && now_ns() - t0 < 4000000000ull) ms(1);
    CHECK(h_os_Filled(&t) == 0 && now_ns() - t1 >= 10000000ull, "all played, in real time (%.1f ms after the first pull)", (now_ns() - t1) / 1e6);
    ms(20);
    st = h_os_Stats(&t, 1);
    CHECK(st.underrun > 0 && atomic_load(&g_sig) > 0, "underruns once empty (%lld), the event signaled %d times", st.underrun, atomic_load(&g_sig));
    h_os_Stop(&t); CHECK(h_os_State(&t) == 1, "stopped");
    CHECK(h_os_Write(&t, fbuf, 480) == 480, "write while stopped");
    ms(30);
    CHECK(h_os_Filled(&t) == 480, "nothing played while stopped");
    h_os_Close(&t);
    CHECK(!t.impl && h_os_State(&t) == 0 && g_destroyed == evbuf[0], "closed: Impl pointer cleared, the data event destroyed");
    h_os_Close(&never);   // nothing, as the SDK
    printf("t_audio_stream: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
