// The set of CPU-written textures (mtl_tex_cpu_written, tex_cpu_written, mtl_tex_release): membership, no duplicates, a released texture leaving it, and lookups of textures
// not in it staying cheap with thousands of consecutive serials in it (a CFSet that hashed serials as numbers probed a whole bucket run per miss: half of Batman: Arkham Knight's
// rendering thread with 6,442 CPU-written textures)
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

static void retired_classification(void) {
    uint8_t mem[2]; pool_t pools[2] = {{.mem=&mem[0]}, {.mem=&mem[1]}};
    tex_t a = {.serial=nvn_new_serial(), .pool=&pools[0], .mem=&mem[0]};
    tex_t b = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_tex_cpu_written(&a); mtl_tex_cpu_written(&b);
    mtl_tex_release(&a);
    CHECK(mtl_cpu_tex_count() == 1 && !nvn_hm_get(&g_cpu_tex,&a.serial), "release removes live membership");
    CHECK(tex_cpu_written(a.serial), "recorded snapshots retain CPU classification");
    uint64_t gen = g_ser_gen; mtl_tex_cpu_written(&a);
    CHECK(mtl_cpu_tex_count() == 1 && g_ser_gen == gen, "late repeated marking cannot resurrect a released texture");
    mtl_alias_drop_pool(&pools[0]);
    CHECK(!tex_cpu_written(a.serial) && tex_cpu_written(b.serial), "pool cleanup preserves the other pool's live membership");
    // A poll may first discover CPU writes after another thread finalized the texture.
    tex_t late = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&late); mtl_tex_release(&late); gen = g_ser_gen; mtl_tex_cpu_written(&late);
    CHECK(tex_cpu_written(late.serial) && mtl_cpu_tex_count() == 1 && g_ser_gen == gen+1, "first marking after release records only retired classification");
    mtl_tex_release(&b); mtl_tex_release(&b);
    CHECK(mtl_cpu_tex_count() == 0 && tex_cpu_written(b.serial), "repeated release keeps retired snapshots classified");
    mtl_alias_drop_pool(&pools[1]);
    CHECK(!tex_cpu_written(b.serial) && !tex_cpu_written(late.serial) && !g_cpu_tex_retired.n, "pool cleanup removes all retired classifications");
}
// A pool that holds both kinds: an uncompressed texture is kept from its storage except where a host texture holds what the GPU wrote there (Ocarina of Time's height tiles beside textures the GPU writes)
static void gpu_written_ranges(void) {
    enum { LEN = 4 << 20, FAR = 2 << 20 };
    uint8_t *mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (mem == MAP_FAILED) abort();
    memset(mem, 0x5a, LEN);
    static pool_t pool = {.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED}; pool.mem = mem; pool.size = LEN;
    tex_t a = {.magic=MAGIC('T','E','X',0), .serial=nvn_new_serial(), .pool=&pool, .mem=mem}, b = a; b.serial = nvn_new_serial(); b.mem = mem + FAR; b.off = FAR;
    a.d = b.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_R32F, .width=260, .height=260, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&a.d, &a.L); b.L = a.L;
    mtl_tex_gpu_written(&pool, mem + FAR, a.L.size);
    CHECK(tex_cpu_content(&a), "a texture beside what the GPU wrote is taken from its storage, in a pool the GPU writes");
    alias_key k = tex_alias_key(&b); nvn_hm_put(&g_alias, &k, &pool);
    CHECK(!tex_cpu_content(&b), "the one over it is not, while the sharing table holds the GPU's texture");
    nvn_hm_del(&g_alias, &k);
    atomic_store(&g_pool_gpuw_new, 1); cpu_unc_drop();
    CHECK(nvn_hm_get(&g_cpu_unc, &a.serial) != NULL, "a write elsewhere in the pool leaves it taken");
    mtl_tex_gpu_written(&pool, mem + 16, 16); cpu_unc_drop();
    CHECK(!nvn_hm_get(&g_cpu_unc, &a.serial) && !g_cpu_unc_lo.n && !g_cpu_unc_sz.n, "a write over its storage ends that");
    CHECK(gpuw_overlaps((uint64_t)(uintptr_t)mem + 20, (uint64_t)(uintptr_t)mem + 21) && !gpuw_overlaps((uint64_t)(uintptr_t)mem + 32, (uint64_t)(uintptr_t)mem + 64), "the ranges are exact");
    uint64_t base = (uint64_t)(uintptr_t)mem; uint32_t n = g_ngpuw;
    CHECK(gpuw_add(base + 8, base + 40) && g_ngpuw == n && !gpuw_add(base + 20, base + 30), "ranges merge, and one inside another is nothing new");
    gpuw_add(base + 100, base + 200); gpuw_drop(base + 120, base + 130);
    CHECK(gpuw_overlaps(base + 110, base + 111) && gpuw_overlaps(base + 150, base + 151) && !gpuw_overlaps(base + 125, base + 126), "dropping the middle of a range leaves both ends");
    CHECK(gpuw_overlaps(base + FAR, base + FAR + 1) && tex_cpu_content(&b) && !gpuw_overlaps(base + FAR, base + FAR + b.L.size), "over a range no host texture holds for it: taken, and the range is cut");
    atomic_store(&g_pool_gpuw_new, 1); cpu_unc_drop();
    CHECK(nvn_hm_get(&g_cpu_unc, &b.serial) != NULL, "so it stays taken");
    cpu_tex_drop_pool(&pool);
    CHECK(!gpuw_overlaps(base, base + LEN), "a pool that goes takes its ranges along");
    mtl_tex_gpu_written(&pool, NULL, 0);
    CHECK(!gpuw_overlaps(base, base + LEN), "no storage given: nothing recorded");
    cpu_tex_drop_pool(&pool); munmap(mem, LEN);
}
static void descriptor_replacement(void) {
    tex_t a = {.serial=nvn_new_serial()}, b = {.serial=nvn_new_serial()};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_reg_tex(1,&a,NULL); mtl_reg_tex(2,&a,NULL); mtl_reg_tex(3,&a,NULL);
    mtl_reg_tex(2,&b,NULL); mtl_reg_tex(3,NULL,NULL);
    mtl_tex_release(&a);
    CHECK(g_treg[1].retired && !g_treg[2].retired && !g_treg[3].ok, "replacing middle and head descriptors preserves serial ownership");
    mtl_reg_tex(1,&b,NULL); mtl_reg_tex(2,NULL,NULL);
    CHECK(!nvn_hm_get(&g_treg_serial,&a.serial), "replacing the last descriptor removes the old serial");
    mtl_tex_release(&b);
    CHECK(g_treg[1].retired, "the replacement texture retires its current descriptors");
    mtl_reg_tex(1,NULL,NULL);
    CHECK(!g_treg_serial.n, "clearing descriptors removes serial links");
}
static void finalized_upload(int write_after_record) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture without packaged layout initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    if (!write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    NVNcommandBuffer cmd; CHECK(h_nvnCommandBufferInitialize(&cmd, NULL), "command buffer initialized");
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (uint64_t)mem+131072, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    if (write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    // Cached descriptor views must not own every upload after its API object
    // is finalized. Two IDs and a compatible view exercise all lookup caches.
    mtl_reg_tex(2, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    id<MTLTexture> before = tex_by_id(1), second = tex_by_id(2); uint64_t compatible_id = 0;
    CHECK(before && second && g_treg[1].view && g_treg[2].view, "live descriptors cache their texture");
    CHECK(tex_fit(2, second, MTLTextureType2DArray, &compatible_id) && g_treg[2].alt, "live descriptor caches a compatible view");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "initial descriptor uploads completed");
    h_nvnTextureFinalize(&tex);
    CHECK(mtl_cpu_tex_count() == 0, "finalized texture leaves the live CPU set");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "finalization retires every descriptor's cached views");
    // Reuse the API object: commands and registered descriptors must use their snapshots.
    memset(&tex, 0xa5, sizeof tex);
    state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded texture readback completed");
    CHECK(!mtl_last_commit_error(), "recorded texture readback has no GPU error");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "finalized command snapshot uploads CPU-written pixels");
    mtl_sub_begin(); id<MTLTexture> registered = tex_by_id(1);
    CHECK(registered && registered.width == 4, "finalized registered descriptor resolves a Metal texture");
    CHECK(tex_fit(2, tex_by_id(2), MTLTextureType2DArray, &compatible_id), "late compatible view resolves");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "registered descriptor upload completed");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "late descriptor views retire at submission end");
    // Resolve again: a retired serial may already hold an NvPkg in the ghost cache.
    mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded snapshot can be replayed again");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "replayed retired snapshot keeps its CPU content");
    mtl_reg_tex(1,NULL,NULL); mtl_reg_tex(2,NULL,NULL); h_nvnCommandBufferFinalize(&cmd);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    CHECK(!tex_cpu_written(serial), "pool finalization drops retired CPU classification");
    defer_drain(); munmap(mem,len);
}
static void finalized_bindless_pool(void) {
    enum { N = NGHOST + 16, FIRST = 256, STRIDE = 4096 };
    const size_t len = N * STRIDE;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    NVNmemoryPool pool = {0};
    pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pb), "bindless texture storage initialized");
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    uint8_t pixels[64]; memset(pixels, 0x7e, sizeof pixels);
    int initialized = 1;
    for (int i = 0; i < N; i++) {
        NVNtexture tex = {0};
        h_nvnTextureBuilderSetStorage(&tb, &pool, (ptrdiff_t)i * STRIDE);
        initialized &= h_nvnTextureInitialize(&tex, &tb);
        h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
        mtl_reg_tex(FIRST + i, (const tex_t *)&tex, NULL);
        h_nvnTextureFinalize(&tex);
    }
    CHECK(initialized, "all retired descriptors have valid storage");
    mtl_sub_begin();
    uint64_t uploads = g_pkg_uploads;
    __unsafe_unretained id<MTLBuffer> a = nil, b = nil;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &a);
    uint64_t first = ((const uint64_t *)((const char *)a.contents + DMSH_POOL_HEAD))[FIRST];
    CHECK(g_pkg_uploads == uploads + N && g_pkg_alias.n == N && !g_ghost_active.n, "one upload per retired descriptor in the first pool walk, kept by storage with no ghost entry");
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b);
    uint64_t again = ((const uint64_t *)((const char *)b.contents + DMSH_POOL_HEAD))[FIRST];
    os_unfair_lock_unlock(&g_rmu);
    CHECK(first == again && g_pkg_uploads == uploads + N, "rebuilding the pool preserves textures that spilled out of the ghost ring");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "bindless pool uploads completed");
    CHECK(!g_ghost_active.n && !g_ghost.n && g_pkg_alias.n == N, "the ring holds none of them");
    for (int i = 0; i < N; i++) mtl_reg_tex(FIRST + i, NULL, NULL);
    h_nvnMemoryPoolFinalize(&pool);
    CHECK(!g_pkg_alias.n && !g_pkg_kept_bytes, "the pool's kept textures go with it");
    defer_drain(); munmap(mem, len);
}
// A packaged texture past its object (nvn_mtl.m g_pkg_alias): kept by storage, the serial table's count leaving with it, found by the next object, uploaded again when the CPU wrote it,
// upgraded in place, let go past the limit and under pressure
static void kept_past_object(void) {
    const size_t len = 1 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    NVNmemoryPool pool = {0};
    pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pb), "kept texture storage initialized");
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, &pool, 65536);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    uint8_t pixels[64]; memset(pixels, 0x41, sizeof pixels);
    NVNtexture a = {0}, b = {0};
    CHECK(h_nvnTextureInitialize(&a, &tb), "first object");
    h_nvnTextureWriteTexels(&a, NULL, &region, pixels);
    uint64_t uploads = g_pkg_uploads, serial = ((tex_t *)&a)->serial;
    mtl_sub_begin();
    id<MTLTexture> first = tex_mtl((const tex_t *)&a); id entry = tex_get(serial);
    CHECK(first && g_pkg_uploads == uploads + 1 && [entry isKindOfClass:[NvPkg class]] && serial_table_holds((__bridge void *)entry), "the first object uploads, and the serial table holds it");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "first upload completed");
    h_nvnTextureFinalize(&a);
    CHECK(!tex_get(serial) && g_pkg_alias.n == 1 && !serial_table_holds((__bridge void *)entry), "finalized: kept by storage, and the serial count left with it");
    CHECK(h_nvnTextureInitialize(&b, &tb), "second object over the storage");
    mtl_tex_cpu_written((const tex_t *)&b);
    mtl_sub_begin();
    id<MTLTexture> again = tex_mtl((const tex_t *)&b);
    CHECK(again == first && g_pkg_uploads == uploads + 1 && tex_get(((tex_t *)&b)->serial) == entry && serial_table_holds((__bridge void *)entry), "the next object finds it, with no upload");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "lookup completed");
    memset(pixels, 0x42, sizeof pixels); h_nvnTextureWriteTexels(&b, NULL, &region, pixels);
    uint64_t reuploads = g_pkg_reuploads;
    mtl_sub_begin();
    id<MTLTexture> written = tex_mtl((const tex_t *)&b);
    CHECK(written && written != first && g_pkg_reuploads == reuploads + 1, "the CPU wrote it: uploaded again, not read stale");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "second upload completed");
    tex_t snap = *(tex_t *)&b;
    h_nvnTextureFinalize(&b);
    mtl_sub_begin();
    id<MTLTexture> up = tex_upgrade(&snap, written, MTLTextureUsageRenderTarget, written.pixelFormat);
    CHECK(up != written && pkg_alias_of(&snap) == entry && ((NvPkg *)entry)->tex == up, "a finalized serial's upgrade replaces the kept texture, not only its name");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "upgrade completed");
    NVNtexture c = {0}; h_nvnTextureBuilderSetStorage(&tb, &pool, 131072);
    CHECK(h_nvnTextureInitialize(&c, &tb), "an object over other storage");
    h_nvnTextureWriteTexels(&c, NULL, &region, pixels);
    mtl_sub_begin(); CHECK(tex_mtl((const tex_t *)&c) != nil, "its upload"); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "its upload completed");
    uint64_t gone = g_pkg_kept_gone; g_pkg_kept_limit = 1;
    h_nvnTextureFinalize(&c);
    g_pkg_kept_limit = 0;
    CHECK(g_pkg_alias.n == 1 && g_pkg_kept_gone == gone + 1 && !pkg_alias_of(&snap), "past the limit the oldest goes");
    CHECK(pkg_alias_trim() == 1 && !g_pkg_alias.n && !g_pkg_kept_bytes, "under pressure what no live object holds goes");
    h_nvnMemoryPoolFinalize(&pool);
    defer_drain(); munmap(mem, len);
}
// A pool walk makes no texture for an ID whose texture would start empty, live or retired: the table reads the dummy, as it would read 0. A real use of that storage reaches the table, also for a
// retired ID with another serial, and content in the storage still takes a texture
// A block-compressed texture drawn through a shadow (bcrt_attach) has no render-target usage to say the GPU owns it: a copy from it must not read the storage instead
static void bc_target_owned(void) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 99810};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = 0x42, .width = 16, .height = 16, .depth = 1, .levels = 1, .samples = 1}; nvn_tex_layout(&t.d, &t.L);
    mtl_tex_init(&t);
    mtl_sub_begin(); id<MTLTexture> bc = tex_mtl(&t);
    CHECK(bc && mtl_fmt_block(bc.pixelFormat) && !mtl_tex_gpu_owned(t.serial) && !tex_gpu_touched(t.serial), "a compressed texture nothing drew into is not the GPU's");
    objc_setAssociatedObject(bc, &g_bcrt_gpu_key, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    CHECK(mtl_tex_gpu_owned(t.serial) && tex_gpu_touched(t.serial), "one drawn through a shadow is");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "compressed target submission completed");
    mtl_tex_release(&t);
}
static void walk_leaves_empty(void) {
    enum { N = 64, LIVE = 8, FIRST = 256, STRIDE = 4096, CONTENT = N - 1 };
    const size_t len = (N + 1) * STRIDE;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    memset(mem + (size_t)CONTENT * STRIDE, 0x5a, 64);   // storage with data at creation, without COMPRESSIBLE below: tex_init_fill's content
    NVNmemoryPool pool = {0};
    pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize(&pool, (const NVNmemoryPoolBuilder *)&pb), "render target storage initialized");
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    NVNtexture live[LIVE]; memset(live, 0, sizeof live); int initialized = 1;
    for (int i = 0; i < N; i++) {
        NVNtexture tex = {0}, *o = i >= 1 && i <= LIVE ? &live[i - 1] : &tex;
        h_nvnTextureBuilderSetFlags(&tb, i == CONTENT ? 0 : NVN_TEXTURE_FLAGS_COMPRESSIBLE);
        h_nvnTextureBuilderSetStorage(&tb, &pool, (ptrdiff_t)i * STRIDE);
        initialized &= h_nvnTextureInitialize(o, &tb);
        mtl_reg_tex(FIRST + i, (const tex_t *)o, NULL);
        if (o == &tex) h_nvnTextureFinalize(&tex);
    }
    CHECK(initialized, "descriptors over render target storage have valid storage");
    mtl_sub_begin();
    unsigned shared = g_alias.n; uint64_t left = atomic_load(&g_tex_walk_empty);
    __unsafe_unretained id<MTLBuffer> a = nil, b = nil, c = nil;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &a);
    uint32_t n = *(const uint32_t *)a.contents; const uint64_t *e = (const uint64_t *)((const char *)a.contents + DMSH_POOL_HEAD);
    int dummies = 0; for (int i = 0; i < N - 1; i++) dummies += e[FIRST + i] == e[n];
    CHECK(dummies == N - 1 && atomic_load(&g_tex_walk_empty) == left + N - 1, "a walk leaves live and retired IDs of empty textures on the dummy");
    CHECK(g_alias.n == shared + 1 && e[FIRST + CONTENT] != e[n], "a walk makes only the texture with content in its storage");
    g_ser_gen++; pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &b);
    CHECK(g_alias.n == shared + 1, "a walk in a new epoch still makes none");
    os_unfair_lock_unlock(&g_rmu);
    // the storage's first real use: a live object over retired ID 0's storage, and a live ID's own object
    NVNtexture real = {0};
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_COMPRESSIBLE); h_nvnTextureBuilderSetStorage(&tb, &pool, 0);
    CHECK(h_nvnTextureInitialize(&real, &tb), "a new object over the retired descriptor's storage");
    id<MTLTexture> used = tex_mtl((const tex_t *)&real), own = tex_mtl((const tex_t *)&live[0]);
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &c);
    e = (const uint64_t *)((const char *)c.contents + DMSH_POOL_HEAD);
    CHECK(used && e[FIRST] == used.gpuResourceID._impl, "a retired ID reaches the texture a real use made over its storage");
    CHECK(own && e[FIRST + 1] == own.gpuResourceID._impl, "a live ID reaches its object's texture once it is used");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the walks' submission completed");
    h_nvnTextureFinalize(&real); for (int i = 0; i < LIVE; i++) h_nvnTextureFinalize(&live[i]);
    for (int i = 0; i < N; i++) mtl_reg_tex(FIRST + i, NULL, NULL);
    h_nvnMemoryPoolFinalize(&pool);
    defer_drain(); munmap(mem, len);
}
// A texture whose serial left the CPU-written set while its packaged entry is still the texture table's (tex_from_storage is then false): tex_mtl gave the NvPkg itself out as the
// MTLTexture, and tex_by_id's base.buffer threw "-[NvPkg buffer]: unrecognized selector" (upstream #1299, a crash under a mesh task draw). It hands out the entry's texture, as tex_made does
static void packaged_entry_outside_the_set(void) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    id<MTLTexture> first = tex_by_id(1);
    CHECK(first && first.width == 4, "the packaged texture resolves");
    id held = tex_get(serial);
    CHECK([held isKindOfClass:[NvPkg class]], "its table entry is an NvPkg");
    pthread_mutex_lock(&g_cpu_tex_mu); nvn_hm_del(&g_cpu_tex, &serial); pthread_mutex_unlock(&g_cpu_tex_mu);   // the serial leaves the set, its entry stays (cpu_unc_drop between its deletes and its tex_put)
    CHECK(!tex_cpu_written(serial), "the serial is no longer CPU-written");
    id<MTLTexture> again = tex_mtl((const tex_t *)&tex);
    CHECK(again && ![(id)again isKindOfClass:[NvPkg class]], "tex_mtl does not hand out the NvPkg");
    CHECK(again && again.width == 4 && again.height == 4, "it hands out the entry's texture");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "submission completed");
    mtl_reg_tex(1,NULL,NULL); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    defer_drain(); munmap(mem,len);
}
// nvnMemoryPoolFinalize on a game thread while the submit thread walks the texture IDs (pool_gpu, holding g_rmu): the finalize dropped the pool's CPU-written classification before it
// marked the pool's IDs gone, so a walk in between found a live ID, a pool still set and a texture no longer CPU-written, and tex_mtl_ handed out its packaged entry (an NvPkg) as the
// texture: "-[NvPkg buffer]: unrecognized selector" in tex_by_id (Ocarina of Time, reference/nvn-objects.md). Here the test holds g_rmu as the walk does while another thread finalizes
static void *finalize_pool(void *pool) { h_nvnMemoryPoolFinalize((NVNmemoryPool *)pool); return NULL; }
static void finalize_during_walk(void) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> first = tex_by_id(1);
    CHECK(first && first.width == 4, "the packaged texture resolves");
    CHECK([tex_get(serial) isKindOfClass:[NvPkg class]], "its table entry is an NvPkg");
    pthread_t th; pthread_create(&th, NULL, finalize_pool, &pool);
    usleep(100000);   // the finalize runs as far as it can while the walk holds g_rmu
    CHECK(tex_cpu_written(serial), "the pool's classification stays while a walk can still find its IDs live");
    id again = tex_mtl((const tex_t *)&tex);
    CHECK(again && ![again isKindOfClass:[NvPkg class]], "tex_mtl does not hand out the NvPkg during the finalize");
    os_unfair_lock_unlock(&g_rmu);
    pthread_join(th, NULL);
    CHECK(!tex_cpu_written(serial) && g_treg[1].gone, "after the finalize the ID is gone and the classification dropped");
    os_unfair_lock_lock(&g_rmu); id after = tex_by_id(1); os_unfair_lock_unlock(&g_rmu);
    CHECK(![after isKindOfClass:[NvPkg class]], "the gone ID does not hand out the NvPkg either");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "submission completed");
    mtl_reg_tex(1,NULL,NULL); h_nvnTextureFinalize(&tex);
    defer_drain(); munmap(mem,len);
}
int main(int argc, const char **argv) { @autoreleasepool {
    enum { N = 6442, BASE = 1000 };
    static tex_t t[N];
    for (int i = 0; i < N; i++) { t[i].serial = BASE + i; t[i].d.width = t[i].d.height = 4; t[i].d.depth = 1; t[i].d.format = 0x25; mtl_tex_init(&t[i]); }
    CHECK(mtl_cpu_tex_count() == 0 && !tex_cpu_written(BASE), "empty at start");
    for (int i = 0; i < N; i++) mtl_tex_cpu_written(&t[i]);
    mtl_tex_cpu_written(&t[7]);   // again: no duplicate
    CHECK(mtl_cpu_tex_count() == N, "every CPU-written texture counted once");
    int in = 0; for (int i = 0; i < N; i++) in += tex_cpu_written(BASE + i);
    CHECK(in == N, "every one found");
    CHECK(!tex_cpu_written(BASE - 1) && !tex_cpu_written(BASE + N) && !tex_cpu_written(0), "serials outside the set are not in it");
    // Misses: a million lookups of serials past the set, as every resolve of a GPU-only texture asks. With the old set's probing this took seconds; with a mixed hash, far less
    uint64_t t0 = bp_now(); int hits = 0;
    for (int i = 0; i < 1000000; i++) hits += tex_cpu_written(BASE + N + 1 + (uint64_t)i);
    double ms = (bp_now() - t0) / 1e6;
    printf("1,000,000 lookups of serials not in a set of %d consecutive ones: %.1f ms\n", N, ms);
    CHECK(hits == 0, "no miss is reported as a hit");
    CHECK(ms < 1000, "a million misses take well under a second");
    mtl_tex_release(&t[100]); mtl_tex_release(&t[101]);
    CHECK(!tex_cpu_written(BASE + 100) && !tex_cpu_written(BASE + 101) && tex_cpu_written(BASE + 102), "a released texture leaves the set, the others stay");
    CHECK(mtl_cpu_tex_count() == N - 2, "the count follows the releases");
    tex_t z = {0}; mtl_tex_cpu_written(&z);
    CHECK(mtl_cpu_tex_count() == N - 2, "a texture with no serial is not recorded");
    for (int i=0;i<N;i++) mtl_tex_release(&t[i]);
    // NVNtexture is plain data: finalizing a copy ends the copy. The texture stays live while the object it was initialized in holds it; if that object no longer does, the copy was the last one
    static tex_t own, cpy, moved;
    own.magic = MAGIC('T', 'E', 'X', 0); own.serial = 900001; own.d = t[0].d; mtl_tex_init(&own); mtl_tex_cpu_written(&own);
    cpy = own; mtl_tex_release(&cpy);
    CHECK(tex_cpu_written(own.serial) == 1, "finalizing a copy leaves the texture live");
    mtl_tex_release(&own);
    CHECK(!nvn_hm_get(&g_tex_live, &own.serial), "finalizing the object itself ends it");
    own.serial = 900002; mtl_tex_init(&own); moved = own; own.magic = 0; mtl_tex_release(&moved);
    CHECK(!nvn_hm_get(&g_tex_live, &moved.serial), "a copy outliving the object it was initialized in ends the texture");
    retired_classification(); descriptor_replacement(); gpu_written_ranges();
    if (argc > 1 && !strcmp(argv[1], "--metal")) { mtl_init(); finalized_upload(0); finalized_upload(1); finalize_during_walk(); finalized_bindless_pool(); kept_past_object(); bc_target_owned(); walk_leaves_empty(); packaged_entry_outside_the_set(); }
    printf("t_cpu_tex: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
