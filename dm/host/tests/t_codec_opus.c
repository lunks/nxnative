// SDK 21's hardware Opus decoder manager as the host serves it (hle_codec.c), without a guest: the work buffer layout, decoder resources, io buffers, the request queues
// (waiting -> executing -> completed) with their counts, the event, Reset's effect on the next request only, Finalize, and the C interface sharing Finalize / Reset.
// The sdk's software OpusDecoder is a stand-in that checks the 8-byte packet header and writes the packet's first payload byte (plus 0x100 after a reset) as every sample.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_codec.c"
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static struct { const void *k; void *v; } om[64];
void *objmap_get(const void *k) { for (int i = 0; i < 64; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 64; i++) if (om[i].k == k || !om[i].k) { om[i].k = k; om[i].v = v; return; } abort(); }
void *objmap_del(const void *k) { for (int i = 0; i < 64; i++) if (om[i].k == k) { om[i].k = NULL; return om[i].v; } return NULL; }
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
static void *bound_ev; static int bound_auto, signals;
void hle_bind_system_event(void *ev, int autoclear) { bound_ev = ev; bound_auto = autoclear; }
void hle_signal_system_event(void *ev) { assert(ev == bound_ev); signals++; }

// The software decoder stand-in: its state in the OpusDecoderType block
typedef struct { int rate, ch, reset, inits, finals; void *buf; size_t size; } soft_t;
#define SOFT_WORK(ch) (0x123u * (unsigned)(ch))   // deliberately not a multiple of 64
static size_t s_size(int rate, int ch) { (void)rate; return SOFT_WORK(ch); }
static int s_init(void *s, int rate, int ch, void *buf, size_t size) {
    soft_t *o = s; if (size < SOFT_WORK(ch)) return 3;
    memset(buf, 0xab, size);   // the whole work buffer is the decoder's: it must not overlap another decoder's or an io buffer
    o->rate = rate; o->ch = ch; o->buf = buf; o->size = size; o->reset = 0; o->inits++; return 0;
}
static int finals; static void s_fini(void *s) { ((soft_t *)s)->finals++; finals++; }
static int resets; static void s_reset(void *s) { ((soft_t *)s)->reset = 1; resets++; }
static int s_rate(const void *s) { return ((const soft_t *)s)->rate; }
static int s_decode(void *s, size_t *used, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    soft_t *o = s; const uint8_t *p = in;
    if (in_size < 8) return 6;
    size_t n = 8 + opus_be32(p); if (n > in_size || n < 9) return 6;
    if (p[8] == 0xee) return 4;   // a broken packet
    int count = o->rate / 50;    // 20 ms
    if (out_size < (size_t)count * (size_t)o->ch * 2) return 7;
    for (int i = 0; i < count * o->ch; i++) out[i] = (int16_t)(p[8] | (o->reset ? 0x100 : 0));
    o->reset = 0; *used = n; *samples = count; return 0;
}
void *guest_sym(const char *n) {
    static const struct { const char *n; void *f; } t[] = {
        {"_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii", (void *)s_size},
        {"_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm", (void *)s_init},
        {"_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE", (void *)s_fini},
        {"_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE", (void *)s_reset},
        {"_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE", (void *)s_rate},
        {"_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm", (void *)s_decode},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(t[i].n, n)) return t[i].f;
    return NULL;
}

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
#define MS20 20000000ll
static size_t packet(uint8_t *b, uint8_t value, unsigned payload) {   // header (big-endian size, final range) and payload
    b[0] = 0; b[1] = 0; b[2] = (uint8_t)(payload >> 8); b[3] = (uint8_t)payload; memset(b + 4, 0x5a, 4);
    memset(b + 8, value, payload); return 8 + payload;
}
static int enqueue(void *dec, uint8_t value) { uint8_t b[64]; size_t used = 0; size_t n = packet(b, value, 17); int r = h_codec_HwOpusEnqueue(dec, &used, b, n); if (!r && used != n) return -1; return r; }
static int dequeue_value(void *dec, int ch) {   // -1: wrong sample count; -2: samples differ
    int16_t pcm[960 * 2]; int n = -7;
    int r = h_codec_HwOpusDequeue(dec, &n, pcm, sizeof pcm);
    if (r) return -100 - r;
    if (n != 960) return -1;
    for (int i = 1; i < 960 * ch; i++) if (pcm[i] != pcm[0]) return -2;
    return pcm[0];
}
static int aborts(void (*fn)(void *), void *arg) {   // the call ends in abort(), as the SDK's own path does
    pid_t c = fork(); if (c < 0) return 0;
    if (!c) { struct rlimit r = {0, 0}; setrlimit(RLIMIT_CORE, &r); if (!freopen("/dev/null", "w", stderr)) _exit(2); fn(arg); _exit(0); }
    int s; waitpid(c, &s, 0); return WIFSIGNALED(s) && WTERMSIG(s) == SIGABRT;
}
static void dequeue_empty(void *dec) { int16_t pcm[4]; int n; h_codec_HwOpusDequeue(dec, &n, pcm, sizeof pcm); }
static void enqueue_short(void *dec) { uint8_t b[16]; size_t n = packet(b, 1, 8), used; h_codec_HwOpusEnqueue(dec, &used, b, n - 1); }
static void enqueue_big(void *dec) { static uint8_t b[0x700]; size_t n = packet(b, 1, 0x600 - 7), used; h_codec_HwOpusEnqueue(dec, &used, b, n); }
static void dequeue_small(void *dec) { int16_t pcm[960]; int n; h_codec_HwOpusDequeue(dec, &n, pcm, sizeof pcm); }   // a mono buffer for stereo PCM

int main(void) {
    // Work buffer: a software work buffer per decoder resource, then the io buffers (0x600 input bytes and one frame of PCM each), from a 64-byte boundary
    const int NDEC = 2, NIO = 3;
    size_t want = 63 + (size_t)NDEC * opus_a64(SOFT_WORK(2)) + (size_t)NIO * opus_a64(0x600 + 960 * 2 * 2);
    size_t size = h_codec_HwOpusMgrSize(48000, 2, MS20, NDEC, NIO);
    CHECK(size == want, "work buffer size: software decoders and io buffers");
    CHECK(h_codec_HwOpusMgrSize(48000, 1, MS20, 128, 255) < h_codec_HwOpusMgrSize(48000, 2, MS20, 128, 255), "mono needs less than stereo");
    uint8_t *buf = malloc(size + 64), *work = buf + 1;   // an unaligned buffer: the layout starts at the next 64-byte boundary
    _Alignas(16) static uint8_t mgr[0x18], ev[0x30], dec[3][0x30], cdec[0x30];
    CHECK(h_codec_HwOpusMgrInit(mgr, ev, 48000, 2, MS20, NDEC, NIO, work, size) == 0, "manager initialized");
    CHECK(bound_ev == ev && bound_auto == 1, "the game's event bound, auto-clear as the SDK attaches it");
    CHECK(h_codec_HwOpusMgrDecTotal(mgr) == NDEC && h_codec_HwOpusMgrDecFree(mgr) == NDEC && h_codec_HwOpusMgrDecUsing(mgr) == 0, "decoder resource counts");
    CHECK(h_codec_HwOpusMgrIoTotal(mgr) == NIO && h_codec_HwOpusMgrIoFree(mgr) == NIO && h_codec_HwOpusMgrIoUsing(mgr) == 0, "io buffer counts");
    CHECK(h_codec_HwOpusMgrFits(mgr, 48000, 2, MS20) && h_codec_HwOpusMgrFits(mgr, 48000, 1, MS20 / 2), "parameters within the manager's fit");
    CHECK(!h_codec_HwOpusMgrFits(mgr, 48000, 2, 40000000ll) && !h_codec_HwOpusMgrFits(mgr, 44100, 2, MS20) && !h_codec_HwOpusMgrFits(mgr, 48000, 3, MS20), "invalid parameters do not");

    // Decoder resources: two, the third gets 0xd
    CHECK(h_codec_HwOpusInitM(dec[0], 48000, 2, MS20, mgr) == 0 && h_codec_HwOpusInitM(dec[1], 48000, 2, MS20, mgr) == 0, "two decoders");
    CHECK(h_codec_HwOpusInitM(dec[2], 48000, 2, MS20, mgr) == 0xd && !objmap_get(dec[2]), "no free resource: 0xd");
    CHECK(h_codec_HwOpusMgrDecFree(mgr) == 0 && h_codec_HwOpusMgrDecUsing(mgr) == 2, "resources in use");
    CHECK(h_codec_HwOpusGetMgr(dec[0]) == mgr && h_codec_HwOpusFrame(dec[1]) == MS20 && h_codec_HwOpusRate(dec[0]) == 48000, "decoder getters");
    soft_t *s0 = objmap_get(dec[0]), *s1 = objmap_get(dec[1]);
    uint8_t *base = (uint8_t *)(((uintptr_t)work + 63) & ~(uintptr_t)63);
    CHECK(s0->buf == base && s1->buf == base + opus_a64(SOFT_WORK(2)) && s0->size == SOFT_WORK(2), "software work buffers in the decoder resources, in order");

    // Enqueue: one packet per request, header + size consumed; the trailing input stays the caller's
    uint8_t two[128]; size_t n1 = packet(two, 1, 20); size_t n2 = packet(two + n1, 9, 30), used = 0;
    CHECK(h_codec_HwOpusEnqueue(dec[0], &used, two, n1 + n2) == 0 && used == n1, "the first packet of the input");
    CHECK(h_codec_HwOpusWaiting(dec[0]) == 1 && h_codec_HwOpusMgrIoFree(mgr) == 2 && h_codec_HwOpusMgrIoUsing(mgr) == 1, "a waiting request takes an io buffer");
    memset(two, 0, sizeof two);   // the request keeps its own copy
    CHECK(enqueue(dec[1], 3) == 0 && enqueue(dec[0], 2) == 0, "more requests");
    used = 77; uint8_t b[64]; size_t n = packet(b, 4, 10);
    CHECK(h_codec_HwOpusEnqueue(dec[1], &used, b, n) == 0xe && used == 77, "no free io buffer: 0xe, nothing consumed");
    CHECK(h_codec_HwOpusCompleted(dec[0]) == 0 && h_codec_HwOpusExecuting(dec[0]) == 0, "nothing executes before Execute");

    // Execute: all waiting requests decode, in order per decoder, and the event is signalled once
    h_codec_HwOpusMgrExec(mgr);
    CHECK(signals == 1, "Execute signals the event");
    CHECK(h_codec_HwOpusWaiting(dec[0]) == 0 && h_codec_HwOpusExecuting(dec[0]) == 0 && h_codec_HwOpusCompleted(dec[0]) == 2 && h_codec_HwOpusCompleted(dec[1]) == 1, "requests completed");
    CHECK(h_codec_HwOpusMgrIoFree(mgr) == 0, "completed requests keep their io buffers");
    h_codec_HwOpusMgrExec(mgr);
    CHECK(signals == 1, "an Execute with nothing to do signals nothing");
    int v1 = dequeue_value(dec[0], 2), v2 = dequeue_value(dec[0], 2), v3 = dequeue_value(dec[1], 2);
    CHECK(v1 == 1 && v2 == 2 && v3 == 3, "Dequeue: oldest first, 960 samples, the decoded PCM");
    CHECK(h_codec_HwOpusCompleted(dec[0]) == 0 && h_codec_HwOpusMgrIoFree(mgr) == 3, "dequeued io buffers are free again");

    // Reset: only the requests enqueued after it decode from a reset state
    CHECK(enqueue(dec[0], 5) == 0, "request before Reset");
    h_codec_HwOpusReset(dec[0]);
    CHECK(resets == 0, "Reset only flags the manager's decoder");
    CHECK(enqueue(dec[0], 6) == 0 && enqueue(dec[0], 7) == 0, "requests after Reset");
    h_codec_HwOpusMgrExec(mgr);
    CHECK(resets == 1 && signals == 2, "the reset reached the decoder at its request");
    v1 = dequeue_value(dec[0], 2); v2 = dequeue_value(dec[0], 2); v3 = dequeue_value(dec[0], 2);
    CHECK(v1 == 5 && v2 == (6 | 0x100) && v3 == 7, "the reset applies to the next request only");

    // A failed decode: Dequeue returns its result and leaves the sample count alone; a NULL buffer drops the PCM
    CHECK(enqueue(dec[1], 0xee) == 0 && enqueue(dec[1], 8) == 0, "a broken packet and a good one");
    h_codec_HwOpusMgrExec(mgr);
    int16_t pcm[960 * 2]; int samples = -5;
    CHECK(h_codec_HwOpusDequeue(dec[1], &samples, pcm, sizeof pcm) == 4 && samples == -5, "the decode result, samples untouched");
    CHECK(h_codec_HwOpusDequeue(dec[1], &samples, NULL, 0) == 0 && samples == 960 && h_codec_HwOpusCompleted(dec[1]) == 0, "no buffer: dropped, counted");
    CHECK(h_codec_HwOpusMgrIoFree(mgr) == 3, "all io buffers back");

    // The SDK's aborts stay aborts
    CHECK(aborts(dequeue_empty, dec[0]), "Dequeue with nothing completed aborts");
    CHECK(aborts(enqueue_short, dec[0]), "a request without a whole packet aborts");
    CHECK(aborts(enqueue_big, dec[0]), "a packet larger than an io buffer aborts");
    CHECK(enqueue(dec[0], 1) == 0, "request for the small buffer");
    h_codec_HwOpusMgrExec(mgr);
    CHECK(aborts(dequeue_small, dec[0]), "a PCM buffer too small aborts");
    CHECK(dequeue_value(dec[0], 2) == 1, "the request is still there for a large enough buffer");

    // Finalize a decoder with waiting and completed requests: its io buffers and its resource come back, the other decoder's requests stay
    CHECK(enqueue(dec[1], 1) == 0 && enqueue(dec[0], 2) == 0, "completed requests on both decoders");
    h_codec_HwOpusMgrExec(mgr);
    CHECK(enqueue(dec[1], 3) == 0, "a waiting request on dec[1]");
    int f = finals;
    h_codec_HwOpusFini(dec[0]);
    CHECK(finals == f + 1 && !objmap_get(dec[0]), "the software decoder finalized, the host state gone");
    CHECK(h_codec_HwOpusMgrIoFree(mgr) == 1 && h_codec_HwOpusMgrDecFree(mgr) == 1, "its io buffer and resource back");
    CHECK(h_codec_HwOpusInitM(dec[2], 48000, 1, MS20, mgr) == 0 && ((soft_t *)objmap_get(dec[2]))->buf == base, "the freed resource serves a new (mono) decoder");
    CHECK(enqueue(dec[2], 4) == 0, "a request on the new decoder");
    h_codec_HwOpusMgrExec(mgr);
    v1 = dequeue_value(dec[1], 2); v2 = dequeue_value(dec[1], 2); v3 = dequeue_value(dec[2], 1);
    CHECK(v1 == 1 && v2 == 3 && v3 == 4, "the others' requests kept their order");
    h_codec_HwOpusFini(dec[1]); h_codec_HwOpusFini(dec[2]);
    CHECK(h_codec_HwOpusMgrDecFree(mgr) == NDEC && h_codec_HwOpusMgrIoFree(mgr) == NIO, "everything free");
    h_codec_HwOpusMgrFini(mgr);
    CHECK(!objmap_get(mgr), "manager finalized");

    // The C interface shares Finalize and Reset with the manager's decoders: a direct software Reset, no manager
    CHECK(h_codec_HwOpusInit(cdec, 48000, 2, work, size, 0) == 0, "C interface decoder");
    int r0 = resets;
    h_codec_HwOpusReset(cdec);
    CHECK(resets == r0 + 1 && h_codec_HwOpusGetMgr(cdec) == NULL, "its Reset goes to the software decoder at once");
    f = finals; h_codec_HwOpusFini(cdec);
    CHECK(finals == f + 1 && !objmap_get(cdec), "its Finalize");
    free(buf);
    printf("t_codec_opus: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
