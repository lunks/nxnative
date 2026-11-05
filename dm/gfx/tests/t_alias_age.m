// Sharing entries that outlived their storage (nvn_mtl.m alias_age): an idle entry nothing holds goes when something used later overlaps it, and only then. A live object, a retired ID a draw still
// binds, and an entry nothing overlaps stay, the last with what the GPU wrote into it; a pool walk is no use; what a walk made from guest storage stays, and comes back with its content after the
// memory guard let it go
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static pool_t g_pool = {.magic=MAGIC('P','O','O','L')};
static uint64_t g_ser_next = 99700;
static tex_t rt(int64_t off, uint32_t size) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=g_ser_next++, .pool=&g_pool, .off=off};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=size, .height=size, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t);
    return t;
}
static int shared(const tex_t *t) {
    alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d;
    pthread_mutex_lock(&g_smu); int r = hm_get_id(&g_alias, &k) != nil; pthread_mutex_unlock(&g_smu);
    return r;
}
static void settle(void) { mtl_sub_begin(); mtl_sub_commit(); mtl_drain(); defer_drain(); res_commit(); }
static void read_pixels(id<MTLTexture> t, uint32_t *out, unsigned n) {
    id<MTLBuffer> dst = [g_dev newBufferWithLength:n * 4 options:MTLResourceStorageModeShared]; res_add(dst); res_commit();
    mtl_sub_begin(); enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(t.width,t.height,1), dst, 0, t.width * 4, n * 4); mtl_sub_commit(); mtl_drain();
    memcpy(out, dst.contents, n * 4); res_remove(dst); res_commit();
}

// Takeover spared the older entry, as it was seen lately; once something used after it overlaps it and it has been idle for the age, the pass finishes the job. The later entry stays
static void overlap(void) {
    mtl_set_frame(1000);
    tex_t old = rt(0, 64); __weak id<MTLTexture> w_old; mtl_sub_begin(); w_old = tex_mtl(&old); mtl_sub_commit(); mtl_tex_release(&old);
    mtl_set_frame(1010);
    tex_t later = rt(0, 128); __weak id<MTLTexture> w_later; mtl_sub_begin(); w_later = tex_mtl(&later); mtl_sub_commit(); mtl_tex_release(&later);
    CHECK(shared(&old) && shared(&later), "takeover spared the older entry");
    mtl_set_frame(1100); CHECK(alias_age(0) == 0 && shared(&old), "younger than the age: it stays");
    mtl_set_frame(1200); CHECK(alias_age(0) == 1, "the pass lets one go");
    CHECK(!shared(&old) && shared(&later), "the older of the two");
    settle(); CHECK(!w_old && w_later, "its memory is released, the later one's is not");
    mtl_set_frame(5000); CHECK(alias_age(0) == 0 && shared(&later), "the last layer stays, however old");
    mtl_alias_drop_pool(&g_pool);
}
// Nothing overlaps it: on the console its bytes are still in the storage, and here only this texture has them
static void kept_bytes(void) {
    mtl_set_frame(2000);
    tex_t t = rt(1 << 20, 64); mtl_sub_begin(); id<MTLTexture> mt = tex_mtl(&t);
    uint32_t pixels[64 * 64]; for (unsigned i = 0; i < 64 * 64; i++) pixels[i] = 0x10203040u + i;
    id<MTLBuffer> src = [g_dev newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared]; res_add(src);
    enc_b_copy_buffer_texture(blit(), src, 0, 256, sizeof pixels, MTLSizeMake(64,64,1), mt, 0, 0, MTLOriginMake(0,0,0));
    mtl_sub_commit(); mtl_drain(); res_remove(src); res_commit();
    mtl_tex_release(&t); mt = nil;
    mtl_set_frame(9000); CHECK(alias_age(0) == 0 && shared(&t), "an entry nothing overlaps stays");
    tex_t again = t; again.serial = g_ser_next++; mtl_tex_init(&again);   // a later object over the same storage and description
    mtl_sub_begin(); id<MTLTexture> back = tex_mtl(&again); mtl_sub_commit();
    uint32_t got[64 * 64] = {0}; read_pixels(back, got, 64 * 64);
    CHECK(!memcmp(got, pixels, sizeof pixels), "and a later object over its key reads what the GPU wrote");
    mtl_tex_release(&again); mtl_alias_drop_pool(&g_pool);
}
static void live_object(void) {
    mtl_set_frame(3000);
    tex_t live = rt(0, 64); mtl_sub_begin(); __weak id<MTLTexture> w = tex_mtl(&live); mtl_sub_commit();
    mtl_set_frame(3010);
    tex_t over = rt(0, 128); mtl_sub_begin(); tex_mtl(&over); mtl_sub_commit(); mtl_tex_release(&over);
    mtl_set_frame(3500); alias_age(0);
    CHECK(shared(&live), "a live object's entry stays, overlapped and untouched");
    mtl_tex_release(&live);   // finalized: only now does its age start, from the last pass
    mtl_set_frame(3600); CHECK(shared(&live) && alias_age(0) == 0, "and it counts as used up to its last pass");
    mtl_alias_drop_pool(&g_pool); settle(); (void)w;
}
// A retired ID's cached view asks the sharing table nothing, and a draw that binds it keeps the entry; a walk reads every ID and does not
static void retired_id(void) {
    mtl_set_frame(4000);
    tex_t r = rt(0, 64); mtl_reg_tex(5, &r, NULL);
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = (__bridge id<MTLTexture>)tex_fast(5); os_unfair_lock_unlock(&g_rmu); mtl_sub_commit();
    CHECK(v != nil, "the ID resolves");
    mtl_tex_release(&r);
    mtl_set_frame(4010);
    tex_t over = rt(0, 128); mtl_sub_begin(); tex_mtl(&over); mtl_sub_commit(); mtl_tex_release(&over);
    for (uint64_t f = 4100; f <= 4500; f += 100) { mtl_set_frame(f); os_unfair_lock_lock(&g_rmu); tex_fast(5); os_unfair_lock_unlock(&g_rmu); alias_age(0); }
    CHECK(shared(&r), "a retired ID draws keep binding stays");
    mtl_set_frame(4550); mtl_sub_begin(); tex_mtl(&over); mtl_sub_commit();   // the overlapping entry is used after the ID's last bind
    int gone = 0, transient = 0;
    for (uint64_t f = 4600; f <= 4700; f += 50) { mtl_set_frame(f); os_unfair_lock_lock(&g_rmu); pool_entry(0, MTLTextureType2D, 5, &gone, &transient); os_unfair_lock_unlock(&g_rmu); }
    CHECK(alias_age(0) == 1 && !shared(&r), "a walk is no use");
    CHECK(!g_treg[5].view, "the retired ID's view goes with it");
    os_unfair_lock_lock(&g_rmu); uint64_t rid = pool_entry(0, MTLTextureType2D, 5, &gone, &transient); os_unfair_lock_unlock(&g_rmu);
    CHECK(!rid && !shared(&r), "the next walk leaves it empty and makes nothing");
    v = nil; mtl_reg_tex(5, NULL, NULL); mtl_alias_drop_pool(&g_pool);
}
// A walk makes a texture only from guest storage. Aged out, it would be made again at every walk, so it stays; let go under memory pressure, it comes back with its content
static void from_storage(void) {
    const size_t len = 1 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    memset(mem, 0x5a, len);
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "the pool");
    mtl_set_frame(6000);
    tex_t t = rt(0, 64); t.pool = &pool; t.mem = mem; mtl_reg_tex(6, &t, NULL); mtl_tex_release(&t);
    int gone = 0, transient = 0;
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); uint64_t rid = pool_entry(0, MTLTextureType2D, 6, &gone, &transient); os_unfair_lock_unlock(&g_rmu); mtl_sub_commit(); mtl_drain();
    CHECK(rid && shared(&t), "the walk made it from its storage");
    mtl_set_frame(6010);
    tex_t over = rt(0, 128); over.pool = &pool; over.mem = mem; mtl_tex_init(&over); mtl_sub_begin(); tex_mtl(&over); mtl_sub_commit(); mtl_tex_release(&over);
    mtl_set_frame(7000); CHECK(alias_age(0) <= 1 && shared(&t), "a walk's texture stays, overlapped and old");
    pthread_mutex_lock(&g_smu); id ghost = hm_get_id(&g_ghost, &t.serial); CHECK(ghost != nil, "the ghost ring holds it"); obj_drop(ghost); hm_del_id(&g_ghost, &t.serial); pthread_mutex_unlock(&g_smu); ghost = nil;   // as the ring comes round
    CHECK(alias_age(1) >= 1 && !shared(&t), "memory pressure lets it go");
    mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); rid = pool_entry(0, MTLTextureType2D, 6, &gone, &transient); id<MTLTexture> back = (__bridge id<MTLTexture>)g_treg[6].view; os_unfair_lock_unlock(&g_rmu); mtl_sub_commit(); mtl_drain();
    CHECK(rid && back, "the next walk makes it again");
    uint32_t got[64 * 64]; memset(got, 0, sizeof got); read_pixels(back, got, 64 * 64);
    int same = 1; for (unsigned i = 0; i < 64 * 64; i++) same &= got[i] == 0x5a5a5a5au;
    CHECK(same, "with the storage's content");
    back = nil; mtl_reg_tex(6, NULL, NULL); mtl_alias_drop_pool(&pool);
}
// A table of handles traced to no slot, as a draw would take it, and one of its entries
static uint64_t table_entry(int tid, uint64_t *dummy) {
    __unsafe_unretained id<MTLBuffer> b = nil; os_unfair_lock_lock(&g_rmu); pool_gpu(0, MTLTextureType2D, 0, 0, &b); os_unfair_lock_unlock(&g_rmu);
    const uint64_t *e = (const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD); uint32_t n = *(const uint32_t *)b.contents;
    if (dummy) *dummy = e[n];
    return e[tid];
}
// What changes a sharing entry reaches the tables as notes: they are patched, and no table is built whole
static pool_t g_tpool; static uint8_t *g_tmem;
static tex_t nt(int64_t off, uint32_t size, uint32_t format) {   // a render target over real storage: its IDs have storage, and it has nothing a walk could take from it
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=g_ser_next++, .pool=&g_tpool, .off=off, .mem=g_tmem + off};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE, .width=size, .height=size, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t);
    return t;
}
static void notes(void) {
    const size_t len = 16 << 20; g_tmem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (g_tmem == MAP_FAILED) abort();
    pool_builder_t pb = {.mem=g_tmem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&g_tpool, (const NVNmemoryPoolBuilder *)&pb), "the pool");
    uint64_t dummy, builds;
    // Takeover and retired IDs: the ID's view goes at the end of the submission, the table takes the dummy at the next one
    mtl_set_frame(10000);
    tex_t a = nt(0, 64, NVN_FORMAT_RGBA8); mtl_reg_tex(7, &a, NULL);
    mtl_sub_begin(); tex_mtl(&a); CHECK(table_entry(7, &dummy) != dummy, "the table holds the texture"); mtl_sub_commit();
    mtl_tex_release(&a);
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); table_entry(7, NULL); mtl_sub_commit(); }
    mtl_drain();
    mtl_sub_begin(); table_entry(7, NULL); builds = g_stat_pool_builds; tex_t over = nt(0, 128, NVN_FORMAT_RGBA8); tex_mtl(&over); CHECK(!shared(&a), "takeover took the entry"); mtl_sub_commit();
    mtl_sub_begin(); CHECK(table_entry(7, &dummy) == dummy, "the retired ID reads the dummy after its submission"); mtl_sub_commit();
    CHECK(g_stat_pool_builds == builds, "takeover builds no table whole");
    mtl_tex_release(&over); mtl_reg_tex(7, NULL, NULL);
    // A walk left a retired ID empty: a new entry over its key fills it, and over its sRGB twin's too
    mtl_sub_begin();
    tex_t e1 = nt(1 << 20, 64, NVN_FORMAT_RGBA8), e2 = nt(2 << 20, 64, NVN_FORMAT_RGBA8_SRGB);
    mtl_reg_tex(8, &e1, NULL); mtl_reg_tex(9, &e2, NULL); mtl_tex_release(&e1); mtl_tex_release(&e2);
    CHECK(table_entry(8, &dummy) == dummy && table_entry(9, NULL) == dummy, "the walk leaves them empty");
    mtl_sub_commit(); mtl_sub_begin(); table_entry(8, NULL); builds = g_stat_pool_builds;
    tex_t n1 = nt(1 << 20, 64, NVN_FORMAT_RGBA8), n2 = nt(2 << 20, 64, NVN_FORMAT_RGBA8);
    id<MTLTexture> t1 = tex_mtl(&n1), t2 = tex_mtl(&n2);
    CHECK(table_entry(8, NULL) == t1.gpuResourceID._impl, "a new entry over the key reaches the ID left empty");
    CHECK(table_entry(9, NULL) != dummy && ((__bridge id<MTLTexture>)g_treg[9].view).parentTexture == t2, "and over its sRGB twin");
    CHECK(g_stat_pool_builds == builds, "without a whole table");
    mtl_sub_commit(); mtl_drain(); t1 = t2 = nil;
    mtl_tex_release(&n1); mtl_tex_release(&n2); mtl_reg_tex(8, NULL, NULL); mtl_reg_tex(9, NULL, NULL);
    // A live sRGB view keeps its parent's entry from takeover, though the view is all a serial names
    mtl_set_frame(11000);
    tex_t par = nt(3 << 20, 64, NVN_FORMAT_RGBA8), srgb = nt(3 << 20, 64, NVN_FORMAT_RGBA8_SRGB); mtl_reg_tex(10, &srgb, NULL);
    mtl_sub_begin(); tex_mtl(&par); tex_mtl(&srgb); uint64_t was = table_entry(10, &dummy); mtl_sub_commit();
    CHECK(was != dummy, "the table holds the view");
    mtl_tex_release(&par);
    for (int i = 0; i <= ALIAS_IDLE; i++) { mtl_sub_begin(); mtl_sub_commit(); }
    mtl_drain();
    mtl_sub_begin(); tex_t big = nt(3 << 20, 128, NVN_FORMAT_RGBA8); tex_mtl(&big); mtl_sub_commit();
    CHECK(shared(&par), "takeover spares the parent of a live sRGB view");
    mtl_sub_begin(); CHECK(table_entry(10, NULL) == was, "and the view stays in the table"); mtl_sub_commit();
    mtl_tex_release(&big);
    // A retired ID's view with no parent, whose texture left the table: resolving to nothing is no match for a view with no parent (nil == nil kept it, and the table named a texture that had left the residency set)
    tex_t gone = nt(4 << 20, 64, NVN_FORMAT_RGBA8); mtl_reg_tex(12, &gone, NULL);
    mtl_sub_begin(); tex_mtl(&gone); CHECK(table_entry(12, &dummy) != dummy, "the table holds it"); mtl_sub_commit();
    mtl_tex_release(&gone);
    mtl_sub_begin(); CHECK(table_entry(12, NULL) != dummy && g_treg[12].view, "the retired ID has a view of the shared texture"); mtl_sub_commit();
    alias_key gk; tex_alias_key(&gone, &gk);
    mtl_sub_begin(); pthread_mutex_lock(&g_smu); alias_let_go(&gk, 1); g_ser_gen++; pthread_mutex_unlock(&g_smu);   // an epoch, before the sweep has run
    CHECK(table_entry(12, NULL) == dummy, "the table does not name it"); CHECK(!g_treg[12].view, "and the view went"); mtl_sub_commit();
    mtl_reg_tex(12, NULL, NULL);
    mtl_tex_release(&srgb); mtl_reg_tex(10, NULL, NULL); mtl_alias_drop_pool(&g_tpool);
}
// A memory pool added or removed changes no table but for the IDs that found storage in it or lost their pool
static void pool_notes(void) {
    const size_t len = 1 << 20; uint64_t dummy, builds;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    memset(mem, 0x5a, len);
    static pool_t pool = {.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t t = rt(0, 64); t.pool = &pool; t.mem = mem; mtl_reg_tex(11, &t, NULL); mtl_tex_release(&t);
    mtl_sub_begin(); CHECK(table_entry(11, &dummy) == dummy, "an ID whose storage no pool holds reads the dummy"); mtl_sub_commit();
    mtl_sub_begin(); table_entry(11, NULL); builds = g_stat_pool_builds;
    uint8_t *other = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (other == MAP_FAILED) abort();
    mtl_pool_add(other, len); mtl_pool_del(other); table_entry(11, NULL);
    CHECK(g_stat_pool_builds == builds, "a pool coming and going builds no table whole");
    mtl_pool_add(mem, len);
    CHECK(table_entry(11, NULL) != dummy, "a pool that holds the ID's storage reaches the table");
    CHECK(g_stat_pool_builds == builds, "by a note");
    mtl_sub_commit(); mtl_drain();
    mtl_sub_begin(); mtl_alias_drop_pool(&pool); pool.magic = 0; mtl_pool_del(mem); table_entry(11, NULL); mtl_sub_commit(); mtl_drain();
    mtl_sub_begin(); CHECK(table_entry(11, &dummy) == dummy, "an ID that lost its pool leaves the table"); mtl_sub_commit();
    mtl_reg_tex(11, NULL, NULL);
}
// The pass's own cost, at the sizes Ocarina of Time reached: 8,000 serial objects, 2,000 entries in a few layers of one pool
static void cost(void) {
    mtl_set_frame(8000);
    enum { NS = 8000, NE = 2000 }; id<MTLTexture> one = [g_dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]];
    pthread_mutex_lock(&g_smu);
    for (uint64_t i = 0; i < NS; i++) { uint64_t k = (1ull << 40) + i; hm_put_id(&g_by_serial, &k, one); }
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    NSMutableArray *keep = [NSMutableArray array];
    for (uint32_t i = 0; i < NE; i++) { tex_t t = {0}; t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=960, .height=540, .depth=1, .levels=1, .samples=1};
        alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)&g_pool; k.off = (int64_t)(i % 97) * (2 << 20); k.d = t.d; k.d.width += i / 97;
        id<MTLTexture> mt = [g_dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]]; [keep addObject:mt];
        hm_put_id(&g_alias, &k, mt); CFSetAddValue(g_alias_set, (__bridge const void *)mt); nvn_hm_put(&g_alias_used, &k, (void *)(uintptr_t)alias_stamp()); }
    pthread_mutex_unlock(&g_smu);
    uint64_t t0 = bp_now(); alias_age(0); uint64_t t1 = bp_now();
    printf("alias_age over %u serial objects and %u entries: %.2f ms\n", NS, NE, (t1 - t0) / 1e6);
    CHECK((t1 - t0) < 40000000ull, "a pass stays well under a frame or two");
    pthread_mutex_lock(&g_smu); for (uint64_t i = 0; i < NS; i++) { uint64_t k = (1ull << 40) + i; hm_del_id(&g_by_serial, &k); } pthread_mutex_unlock(&g_smu);
    mtl_alias_drop_pool(&g_pool);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_RES_LIMIT_MB", "0", 1);
    mtl_init();
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    overlap(); kept_bytes(); live_object(); retired_id(); from_storage(); notes(); pool_notes(); cost();
    printf("t_alias_age: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
