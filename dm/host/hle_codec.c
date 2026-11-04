// nn::codec::HardwareOpusDecoder: the real device decodes in hardware via the hwopus service (the game's audio and cutscene tracks are all Opus; this is not an online feature).
// Host: the constructor instead constructs the SDK's software OpusDecoder in the same memory (the sdk bundles libopus, native ARM).
// Basis: both are implementations of nn::codec::IOpusDecoder, with the 10 vtable slots identical one by one in order and signature (destructor×2, GetWorkBufferSize, IsInitialized,
// Initialize, Finalize, GetSampleRate, GetChannelCount, Reset, DecodeInterleaved); the game allocates 0x40 bytes and the software object is 0x30 bytes.
#include "hle.h"

HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "hwopus service; replaced by the SDK software OpusDecoder (same IOpusDecoder virtual interface)",
          void, h_codec_HwOpusCtor, void *self, int option) {
    (void)option;
    static void (*soft_ctor)(void *);
    if (!soft_ctor && !(soft_ctor = guest_sym("_ZN2nn5codec11OpusDecoderC1Ev"))) die("missing the OpusDecoder constructor");
    soft_ctor(self);
}

// The C interface to the same hardware decoder (Tears of the Kingdom, SDK 20.5.22: GetHardwareOpusDecoderWorkBufferSize opens hwopus and aborts in sm::Initialize).
// Each call goes to the sdk's software C interface instead. The software decoder state lives in a host block kept per game decoder object (objmap), so the size of the
// game's HardwareOpusDecoderType does not matter; the game's work buffer is sized by GetOpusDecoderWorkBufferSize and handed to the software decoder as is.
#define SOFT_OPUS_STATE 0x200   // bytes for the software OpusDecoderType (larger than the SDK's)
static void *opus_fn(const char *name) { void *f = guest_sym(name); if (!f) die("sdk lacks %s", name); return f; }
static void *opus_soft(const void *hw) { void *s = objmap_get(hw); if (!s) die("Opus decoder %p was not initialized by the host", hw); return s; }
// The host block of a HardwareOpusDecoderType. The C interface and SDK 21's decoder manager (below) initialize the same type and share FinalizeHardwareOpusDecoder,
// ResetHardwareOpusDecoder and the getters, so the block says which one it came from; the software decoder is first, so opus_soft serves both
typedef struct opus_mgr opus_mgr_t;
typedef struct {
    uint8_t soft[SOFT_OPUS_STATE];   // the sdk's software OpusDecoderType
    opus_mgr_t *mgr;                 // the manager it decodes through; NULL: the C interface, which decodes on the caller's thread
    int slot, rate, ch; int64_t frame;   // its decoder resource in the manager, and its parameter (frame: nn::TimeSpan, ns)
    int waiting, executing, completed;   // its decode requests: enqueued, being decoded by an Execute, decoded and not yet dequeued
    int done, done_tail;             // the completed ones, oldest first (io buffer indices, -1: none)
    int reset;                       // ResetHardwareOpusDecoder since the last Enqueue: the next request decodes from a reset state
} opus_dec_t;
static void opus_mgr_release(opus_dec_t *d);   // below: its requests and its decoder resource back to the manager
static void opus_mgr_reset(opus_dec_t *d);
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusWorkSize, int rate, int ch, int option) {
    (void)option; size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
// SDK 11's form without the option (Big Brain Academy: Brain vs. Brain sizes its decoders with it early in nnMain, main+0x31f8e8; the SDK's own opens hwopus and aborts resolving the service)
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEii", "hwopus service; work buffer computed for the software decoder (SDK 11 two-argument version)", size_t, h_codec_HwOpusWorkSize2,
          int rate, int ch) {
    size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", "hwopus service; replaced by the software OpusDecoder", int, h_codec_HwOpusInit,
          void *hw, int rate, int ch, void *buf, size_t size, int option) {
    (void)option; int (*f)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    opus_dec_t *d = objmap_get(hw); if (!d) { if (!(d = calloc(1, sizeof *d))) die("Opus decoder calloc"); objmap_put(hw, d); }
    d->mgr = NULL;
    return f(d->soft, rate, ch, buf, size);
}
HLE_FORCE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder (C interface or SDK 21 manager)", void, h_codec_HwOpusFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    opus_dec_t *d = objmap_get(hw); if (!d) return;
    if (d->mgr) opus_mgr_release(d);
    objmap_del(hw); f(d->soft); free(d);
}
HLE_FORCE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder (C interface or SDK 21 manager)", void, h_codec_HwOpusReset, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    opus_dec_t *d = opus_soft(hw);
    if (d->mgr) opus_mgr_reset(d); else f(d->soft);
}
// The hardware's decode time of the last frame (the type's +0x20 in µs times 1000, SDK 21.4.4 sdk+0x325460): the host never writes it, both kinds decode in software, so none is reported
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderProcessingTimeEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; no hardware decode time", int64_t, h_codec_HwOpusTime, const void *hw) {
    (void)hw; return 0;
}
HLE_FORCE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusDecoder", int, h_codec_HwOpusDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}

// SDK 21's asynchronous form of the same decoder: a HardwareOpusDecoderManager holds decoder resources and io buffers, decoders enqueue packets, Execute starts every one, and the
// game dequeues the PCM later (Star Fox, Switch 2, SDK 21.4.4: GetHardwareOpusDecoderManagerWorkBufferSize at main+0xbc35fb4 opened hwopus itself and nn::diag aborted with 0xe15
// at sdk+0x323180). On the console the manager is a client of the hwacod service. Its model, from the SDK's own implementation (addresses in Star Fox's sdk):
// - the parameter is (sample rate, channels, frame duration), then the decoder resource and io buffer counts (WorkBufferSizeImpl sdk+0x326410, InitializeImpl sdk+0x326500):
//   8 / 12 / 16 / 24 / 48 kHz, 1 or 2 channels, 2.5 / 5 / 10 / 20 ms frames, anything else aborts (ValidateInstanceParameter sdk+0x329e6c); the service gives the size
// - a request is one packet with the 8-byte header (big-endian size, final range) the software decoder also reads (DecodeOpusInterleavedImpl sdk+0x321afc): Enqueue takes
//   the input's first packet, reports header + size consumed (GetFirstInputDataSize sdk+0x328ee0) and copies it into a free io buffer of 0x600 input bytes and
//   frame × rate × channels × 2 PCM bytes (WorkBufferSizeCalculator sdk+0x329a7c). No free io buffer: Enqueue returns 0xe (sdk+0x325b50); no free decoder resource:
//   InitializeHardwareOpusDecoder returns 0xd (sdk+0x325ad0)
// - a request waits from Enqueue, executes from the Execute that sends every request enqueued since the last one, all decoders in order, to the service in one call
//   (Manager::Execute sdk+0x328980), and is completed once the service's result ring has it (UpdateProcessResult sdk+0x328aa0, polled by the counts and Dequeue). The
//   service signals the manager's event: the game's SystemEventType, to which InitializeImpl attaches the service's handle, auto-clear (sdk+0x3288a0)
// - Dequeue takes the oldest completed request: samples = bytes / (2 × channels), the request's decode result, its io buffer back (GetOutputData sdk+0x328130); with none
//   completed, or a PCM buffer too small, the SDK aborts (sdk+0x326210)
// - ResetHardwareOpusDecoder only flags the decoder (sdk+0x327760): the next Enqueue's request carries the reset, the ones before it decode as before (SetInputData
//   sdk+0x328060). Finalizing a decoder gives its waiting and completed io buffers back (sdk+0x327f20)
// Star Fox opens two managers at boot (48 kHz, 1 and 2 channels, 20 ms, 128 and 72 decoders, 255 and 160 io buffers; main+0xbc35f58), runs Execute on both once per audio
// frame (main+0xbc355ec), dequeues 960-sample frames (main+0xbc37200), sleeps 100 µs at a time while a decoder still executes (main+0xbc358e0), never waits on the events
// and destroys them after the managers' Finalize (main+0xbc365b8).
// Host: Execute decodes its requests on the caller's thread with the sdk's software OpusDecoder, in enqueue order, then signals the event (once per Execute with requests).
// The game's work buffer holds a software decoder work buffer per decoder resource and the io buffers, and is sized for them; the lists and counts are host state (objmap)
#define OPUS_IO_IN 0x600        // input bytes of an io buffer, header included (sdk+0x329a7c)
#define OPUS_NO_DECODER 0xd     // InitializeHardwareOpusDecoder: no free decoder resource
#define OPUS_NO_IO_BUFFER 0xe   // EnqueueOpusDataToHardwareOpusDecoder: no free io buffer
typedef struct { int next; opus_dec_t *dec; int reset, result; uint32_t in, out; } opus_io_t;   // a decode request in an io buffer: its list link, packet and PCM bytes
struct opus_mgr {
    pthread_mutex_t mu;     // the lists and counts; never held over guest code
    pthread_mutex_t exec;   // held while an Execute decodes (guest code, so taken with dm_host_lock): one batch at a time, no decoder finalized under one
    const void *guest; void *event;
    int rate, ch; int64_t frame; int ndec, nio, free_dec, free_io;
    size_t soft_size, soft_slot, out_cap, io_slot;
    uint8_t *mem;           // the game's work buffer, from 64 bytes up: ndec software decoder work buffers, then nio io buffers (OPUS_IO_IN input bytes, then out_cap PCM bytes)
    uint8_t *used;          // ndec: the decoder resource is taken
    opus_io_t *io; int free_head, pend, pend_tail;   // nio; the free ones, and the requests enqueued since the last Execute (oldest first; -1: none)
};
void dm_host_lock(pthread_mutex_t *m);   // hle_thread.c
static size_t opus_a64(size_t n) { return (n + 63) & ~(size_t)63; }
static int opus_param_ok(int rate, int ch, int64_t frame) {   // ValidateInstanceParameter's Opus case: the frame in whole µs
    int64_t us = frame / 1000;
    return (rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000) && (ch == 1 || ch == 2) && (us == 2500 || us == 5000 || us == 10000 || us == 20000);
}
static size_t opus_out_cap(int rate, int ch, int64_t frame) { return (size_t)((frame * rate + 999999999) / 1000000000) * (size_t)ch * 2; }   // PCM bytes of one frame (sdk+0x329a44)
static size_t opus_soft_size(int rate, int ch) { size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch); }
static size_t opus_mgr_size(int rate, int ch, int64_t frame, int ndec, int nio) {
    return 63 + (size_t)ndec * opus_a64(opus_soft_size(rate, ch)) + (size_t)nio * opus_a64(OPUS_IO_IN + opus_out_cap(rate, ch, frame));
}
static void opus_mgr_check(int rate, int ch, int64_t frame, int ndec, int nio) {
    if (!opus_param_ok(rate, ch, frame) || ndec < 1 || nio < 0)
        die("Opus decoder manager of %d Hz, %d channel(s), %lld ns frames, %d decoders, %d io buffers: the SDK aborts on it (sdk+0x3264ac; no decoder resource: sdk+0x328964)",
            rate, ch, (long long)frame, ndec, nio);
}
static opus_mgr_t *opus_mgr(const void *g) { opus_mgr_t *m = objmap_get(g); if (!m) die("Opus decoder manager %p was not initialized by the host", g); return m; }
static opus_dec_t *opus_mdec(const void *hw) { opus_dec_t *d = objmap_get(hw); if (!d || !d->mgr) die("Opus decoder %p was not initialized with a manager by the host", hw); return d; }
static uint8_t *opus_io_mem(opus_mgr_t *m, int i) { return m->mem + (size_t)m->ndec * m->soft_slot + (size_t)i * m->io_slot; }
static void opus_io_free(opus_mgr_t *m, int i) { m->io[i].dec = NULL; m->io[i].next = m->free_head; m->free_head = i; m->free_io++; }   // holding mu
static uint32_t opus_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void opus_mgr_release(opus_dec_t *d) {   // FinalizeHardwareOpusDecoder, after any Execute decoding its requests: its waiting and completed io buffers and its resource back
    opus_mgr_t *m = d->mgr;
    dm_host_lock(&m->exec); pthread_mutex_lock(&m->mu);
    int keep = -1, tail = -1;
    for (int i = m->pend, nx; i >= 0; i = nx) {
        nx = m->io[i].next;
        if (m->io[i].dec == d) { opus_io_free(m, i); continue; }
        m->io[i].next = -1; if (tail >= 0) m->io[tail].next = i; else keep = i;
        tail = i;
    }
    m->pend = keep; m->pend_tail = tail;
    for (int i = d->done, nx; i >= 0; i = nx) { nx = m->io[i].next; opus_io_free(m, i); }
    d->done = d->done_tail = -1; d->waiting = d->completed = 0;
    m->used[d->slot] = 0; m->free_dec++; d->mgr = NULL;
    pthread_mutex_unlock(&m->mu); pthread_mutex_unlock(&m->exec);
}
static void opus_mgr_reset(opus_dec_t *d) { pthread_mutex_lock(&d->mgr->mu); d->reset = 1; pthread_mutex_unlock(&d->mgr->mu); }
HLE_FORCE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", "hwacod service (SDK 21 manager); sized for the software decoders and io buffers the host keeps there",
          size_t, h_codec_HwOpusMgrSize, int rate, int ch, int64_t frame, int ndec, int nio) {
    opus_mgr_check(rate, ch, frame, ndec, nio); return opus_mgr_size(rate, ch, frame, ndec, nio);
}
HLE_FORCE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm",
          "hwacod service (SDK 21 manager); host lists, software decoders, the event bound to a host kernel event", int, h_codec_HwOpusMgrInit,
          void *g, void *ev, int rate, int ch, int64_t frame, int ndec, int nio, void *buf, size_t size) {
    opus_mgr_check(rate, ch, frame, ndec, nio);
    size_t need = opus_mgr_size(rate, ch, frame, ndec, nio);
    if (!buf || size < need) die("Opus decoder manager work buffer of %#zx bytes, %#zx needed: the SDK aborts (sdk+0x32894c)", size, need);
    opus_mgr_t *m = calloc(1, sizeof *m);
    if (!m || !(m->used = calloc((size_t)ndec, 1)) || !(m->io = calloc((size_t)nio + 1, sizeof *m->io))) die("Opus decoder manager calloc");
    pthread_mutex_init(&m->mu, NULL); pthread_mutex_init(&m->exec, NULL);
    m->guest = g; m->event = ev; m->rate = rate; m->ch = ch; m->frame = frame; m->ndec = m->free_dec = ndec; m->nio = nio;
    m->soft_size = opus_soft_size(rate, ch); m->soft_slot = opus_a64(m->soft_size); m->out_cap = opus_out_cap(rate, ch, frame); m->io_slot = opus_a64(OPUS_IO_IN + m->out_cap);
    m->mem = (uint8_t *)(((uintptr_t)buf + 63) & ~(uintptr_t)63);
    m->free_head = m->pend = m->pend_tail = -1;
    for (int i = nio - 1; i >= 0; i--) opus_io_free(m, i);
    if (objmap_get(g)) LOG("[RED] codec: Opus decoder manager %p initialized again without Finalize: its earlier host state is dropped", g);
    objmap_put(g, m);
    if (ev) hle_bind_system_event(ev, 1);   // EventClearMode_AutoClear, as the SDK attaches the service's handle
    LOG("codec: Opus decoder manager %p: %d Hz, %d channel(s), %lld us frames, %d decoders, %d io buffers; decoded in software at Execute", g, rate, ch, (long long)(frame / 1000), ndec, nio);
    return 0;
}
HLE_FORCE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host state freed (the game destroys the event)",
          void, h_codec_HwOpusMgrFini, void *g) {
    opus_mgr_t *m = objmap_del(g); if (!m) return;
    if (m->free_dec != m->ndec) { LOG("[RED] codec: Opus decoder manager %p finalized with %d decoders still initialized: its host state is kept for them", g, m->ndec - m->free_dec); return; }
    pthread_mutex_destroy(&m->mu); pthread_mutex_destroy(&m->exec); free(m->used); free(m->io); free(m);
}
HLE_FORCE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); the waiting requests decode here in software, then the event is signalled",
          void, h_codec_HwOpusMgrExec, void *g) {
    int (*dec)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    void (*reset)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    opus_mgr_t *m = opus_mgr(g);
    dm_host_lock(&m->exec);
    pthread_mutex_lock(&m->mu);
    int batch = m->pend; m->pend = m->pend_tail = -1;
    for (int i = batch; i >= 0; i = m->io[i].next) { m->io[i].dec->waiting--; m->io[i].dec->executing++; }
    pthread_mutex_unlock(&m->mu);
    for (int i = batch; i >= 0; i = m->io[i].next) {   // this Execute's alone: Enqueue starts a new list, and a decoder's Finalize waits for exec
        opus_io_t *r = &m->io[i]; uint8_t *in = opus_io_mem(m, i); size_t used = 0; int n = 0;
        if (r->reset) reset(r->dec->soft);
        r->result = dec(r->dec->soft, &used, &n, (int16_t *)(in + OPUS_IO_IN), m->out_cap, in, r->in);
        r->out = r->result ? 0 : (uint32_t)n * (uint32_t)r->dec->ch * 2;
        static int once; if (r->result && !once++) LOG("[RED] codec: an Opus decode request of %u bytes failed with OpusResult %d (first case)", r->in, r->result);
    }
    pthread_mutex_lock(&m->mu);
    for (int i = batch, nx; i >= 0; i = nx) {
        nx = m->io[i].next; opus_dec_t *d = m->io[i].dec;
        m->io[i].next = -1; if (d->done_tail >= 0) m->io[d->done_tail].next = i; else d->done = i;
        d->done_tail = i; d->executing--; d->completed++;
    }
    pthread_mutex_unlock(&m->mu);
    pthread_mutex_unlock(&m->exec);
    if (batch >= 0 && m->event) hle_signal_system_event(m->event);   // no lock held: the signal can hand the core to a waiter
}
static int opus_mgr_count(const void *g, int what) {
    opus_mgr_t *m = opus_mgr(g); pthread_mutex_lock(&m->mu);
    int r = what == 0 ? m->ndec : what == 1 ? m->free_dec : what == 2 ? m->ndec - m->free_dec : what == 3 ? m->nio : what == 4 ? m->free_io : m->nio - m->free_io;
    pthread_mutex_unlock(&m->mu); return r;
}
HLE_FORCE("_ZN2nn5codec54GetHardwareOpusDecoderManagerTotalDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrDecTotal, const void *g) { return opus_mgr_count(g, 0); }
HLE_FORCE("_ZN2nn5codec53GetHardwareOpusDecoderManagerFreeDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrDecFree, const void *g) { return opus_mgr_count(g, 1); }
HLE_FORCE("_ZN2nn5codec54GetHardwareOpusDecoderManagerUsingDecoderResourceCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrDecUsing, const void *g) { return opus_mgr_count(g, 2); }
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderManagerTotalIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrIoTotal, const void *g) { return opus_mgr_count(g, 3); }
HLE_FORCE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrIoFree, const void *g) { return opus_mgr_count(g, 4); }
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderManagerUsingIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusMgrIoUsing, const void *g) { return opus_mgr_count(g, 5); }
// Whether a decoder of this parameter fits the manager. The console asks the service for the instance size and compares it with the manager's (CanInstantiate sdk+0x328b90);
// here it is the software decoder's work buffer and one frame of PCM
HLE_FORCE("_ZN2nn5codec49ValidateHardwareOpusDecoderManagerDecoderResourceEPKNS0_30HardwareOpusDecoderManagerTypeEiiNS_8TimeSpanE", "hwacod service (SDK 21 manager); the host's resource sizes",
          int, h_codec_HwOpusMgrFits, const void *g, int rate, int ch, int64_t frame) {
    opus_mgr_t *m = opus_mgr(g);
    return opus_param_ok(rate, ch, frame) && opus_soft_size(rate, ch) <= m->soft_size && opus_out_cap(rate, ch, frame) <= m->out_cap;
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE",
          "hwacod service (SDK 21 manager); a software OpusDecoder in one of the manager's decoder resources", int, h_codec_HwOpusInitM, void *hw, int rate, int ch, int64_t frame, void *g) {
    int (*f)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    opus_mgr_t *m = opus_mgr(g);
    if (!opus_mgr_count(g, 1)) return OPUS_NO_DECODER;
    if (!opus_param_ok(rate, ch, frame)) die("Opus decoder of %d Hz, %d channel(s), %lld ns frames: the SDK aborts on it (sdk+0x325fa4)", rate, ch, (long long)frame);
    if (opus_soft_size(rate, ch) > m->soft_size || opus_out_cap(rate, ch, frame) > m->out_cap) {
        LOG("[RED] codec: Opus decoder of %d Hz, %d channel(s), %lld ns frames does not fit its manager's resources (%d Hz, %d channel(s), %lld ns); the console's answer is not known: 0xd",
            rate, ch, (long long)frame, m->rate, m->ch, (long long)m->frame);
        return OPUS_NO_DECODER;
    }
    pthread_mutex_lock(&m->mu);
    int slot = -1;
    if (m->free_dec) { for (slot = 0; m->used[slot]; slot++) {} m->used[slot] = 1; m->free_dec--; }
    pthread_mutex_unlock(&m->mu);
    if (slot < 0) return OPUS_NO_DECODER;
    opus_dec_t *d = objmap_get(hw); if (!d) { if (!(d = calloc(1, sizeof *d))) die("Opus decoder calloc"); objmap_put(hw, d); }
    int r = f(d->soft, rate, ch, m->mem + (size_t)slot * m->soft_slot, m->soft_size);
    if (r) {
        LOG("[RED] codec: the software OpusDecoder refused %d Hz, %d channel(s): OpusResult %d", rate, ch, r);
        pthread_mutex_lock(&m->mu); m->used[slot] = 0; m->free_dec++; pthread_mutex_unlock(&m->mu);
        d->mgr = NULL; return r;
    }
    d->mgr = m; d->slot = slot; d->rate = rate; d->ch = ch; d->frame = frame;
    d->waiting = d->executing = d->completed = d->reset = 0; d->done = d->done_tail = -1;
    return 0;
}
HLE_FORCE("_ZN2nn5codec29GetHardwareOpusDecoderManagerEPNS0_23HardwareOpusDecoderTypeE", "hwacod service (SDK 21 manager); host state", void *, h_codec_HwOpusGetMgr, void *hw) {
    opus_dec_t *d = objmap_get(hw); return d && d->mgr ? (void *)d->mgr->guest : NULL;
}
HLE_FORCE("_ZN2nn5codec35GetHardwareOpusDecoderFrameDurationEPKNS0_23HardwareOpusDecoderTypeE", "hwacod service (SDK 21 manager); host state", int64_t, h_codec_HwOpusFrame, const void *hw) {
    return opus_mdec(hw)->frame;
}
HLE_FORCE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", "hwacod service (SDK 21 manager); the first packet into a free io buffer, waiting for Execute",
          int, h_codec_HwOpusEnqueue, void *hw, size_t *consumed, const void *data, size_t size) {
    opus_dec_t *d = opus_mdec(hw); opus_mgr_t *m = d->mgr; const uint8_t *p = data;
    pthread_mutex_lock(&m->mu);
    if (!m->free_io) { pthread_mutex_unlock(&m->mu); return OPUS_NO_IO_BUFFER; }
    size_t n = size < 8 ? 0 : 8 + (size_t)opus_be32(p);
    if (!n || n > size) die("Opus decode request of %zu bytes holds no whole packet: the SDK aborts (GetFirstInputDataSize 0x7dc6f, sdk+0x326124)", size);
    if (n > OPUS_IO_IN) die("Opus packet of %zu bytes with its header, an io buffer takes %#x: the SDK aborts (SetInputData 0x7ea6f, sdk+0x326124)", n, OPUS_IO_IN);
    int i = m->free_head; opus_io_t *r = &m->io[i];
    m->free_head = r->next; m->free_io--;
    memcpy(opus_io_mem(m, i), p, n);
    r->dec = d; r->reset = d->reset; r->in = (uint32_t)n; r->out = 0; r->result = 0; r->next = -1; d->reset = 0;
    if (m->pend_tail >= 0) m->io[m->pend_tail].next = i; else m->pend = i;
    m->pend_tail = i; d->waiting++;
    pthread_mutex_unlock(&m->mu);
    if (consumed) *consumed = n;
    return 0;
}
HLE_FORCE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", "hwacod service (SDK 21 manager); the oldest completed request's PCM and result",
          int, h_codec_HwOpusDequeue, void *hw, int *samples, int16_t *out, size_t out_size) {
    opus_dec_t *d = opus_mdec(hw); opus_mgr_t *m = d->mgr;
    pthread_mutex_lock(&m->mu);
    int i = d->done;
    if (i < 0) die("Dequeue from Opus decoder %p with no completed request: the SDK aborts (GetOutputData 0x7f46f, sdk+0x326210)", hw);
    opus_io_t *r = &m->io[i];
    if (out && r->out > out_size) die("Opus PCM of %u bytes into a buffer of %zu: the SDK aborts (GetOutputData 0x7ec6f, sdk+0x326210)", r->out, out_size);
    if (out) memcpy(out, opus_io_mem(m, i) + OPUS_IO_IN, r->out);   // without a buffer the PCM is dropped, as the SDK does
    int res = r->result;
    if (!res && samples) *samples = (int)(r->out / (2u * (unsigned)d->ch));
    d->done = r->next; if (d->done < 0) d->done_tail = -1;
    d->completed--; opus_io_free(m, i);
    pthread_mutex_unlock(&m->mu);
    return res;
}
static int opus_dec_count(const void *hw, int what) {
    opus_dec_t *d = opus_mdec(hw); pthread_mutex_lock(&d->mgr->mu);
    int r = what == 0 ? d->waiting : what == 1 ? d->executing : d->completed;
    pthread_mutex_unlock(&d->mgr->mu); return r;
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusWaiting, const void *hw) { return opus_dec_count(hw, 0); }
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwacod service (SDK 21 manager); host count (non-zero only during an Execute)",
          int, h_codec_HwOpusExecuting, const void *hw) { return opus_dec_count(hw, 1); }
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwacod service (SDK 21 manager); host count",
          int, h_codec_HwOpusCompleted, const void *hw) { return opus_dec_count(hw, 2); }

// nn::codec::HardwareOpusMultiStreamDecoder (Hyrule Warriors: Age of Calamity, SDK 9.6.0: its sound streams are multi-stream Opus): the same swap as HardwareOpusDecoder above.
// Both classes implement one interface: their 12 vtable slots match one by one (destructor×2, GetWorkBufferSize, IsInitialized, Initialize, Finalize, GetSampleRate, GetChannelCount,
// GetTotalStreamCount, GetStereoStreamCount, Reset, DecodeInterleaved); the hardware object is 0x48 bytes, the software one 0x38. Both destructors are a bare ret.
// Without it the game's KIDS resource loader never finished the first sound stream (asset/data/0x272c6efb.file) and the main thread waited for it forever (main+0x8d9bc8)
HLE_FORCE("_ZN2nn5codec30HardwareOpusMultiStreamDecoderC1Ei", "hwopus service; replaced by the SDK software OpusMultiStreamDecoder (same virtual interface)",
          void, h_codec_HwOpusMsCtor, void *self, int option) {
    (void)option; void (*f)(void *) = opus_fn("_ZN2nn5codec22OpusMultiStreamDecoderC1Ev"); f(self);
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize,
          int rate, int ch, int streams, int stereo) {
    size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}

// The C interface to the hardware multi-stream decoder (Unicorn Overlord, SDK 16.2.4: CRI's Opus streams; GetHardwareOpusMultiStreamDecoderWorkBufferSize opened hwopus and the
// game aborted in sm::Initialize at nnMain). The same swap as the single-stream C interface above: software state per game decoder object, the work buffer sized for the software decoder.
// The software OpusMultiStreamDecoderType is 0x28 bytes (InitializeOpusMultiStreamDecoder writes up to +0x24), well inside SOFT_OPUS_STATE.
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize5,
          int rate, int ch, int streams, int stereo, int option) {
    (void)option; size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}
HLE_FORCE("_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", "hwopus service; replaced by the software OpusMultiStreamDecoder",
          int, h_codec_HwOpusMsInit, void *hw, int rate, int ch, int streams, int stereo, const uint8_t *mapping, void *buf, size_t size, int option) {
    (void)option; int (*f)(void *, int, int, int, int, const uint8_t *, void *, size_t) = opus_fn("_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm");
    void *s = objmap_get(hw); if (!s) { s = calloc(1, SOFT_OPUS_STATE); objmap_put(hw, s); }
    return f(s, rate, ch, streams, stereo, mapping, buf, size);
}
HLE_FORCE("_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE");
    void *s = objmap_del(hw); if (s) { f(s); free(s); }
}
HLE_FORCE("_ZN2nn5codec43GetHardwareOpusMultiStreamDecoderSampleRateEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec45GetHardwareOpusMultiStreamDecoderChannelCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusMultiStreamDecoderTotalStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsTotal, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec50GetHardwareOpusMultiStreamDecoderStereoStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsStereo, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsReset, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE"); f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}
// The hardware's decode time of the last frame (nn::TimeSpan); the software decoder runs inline on the caller's thread, so none is reported
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderProcessingTimeEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; no hardware decode time", int64_t, h_codec_HwOpusMsTime, const void *hw) {
    (void)hw; return 0;
}
