// The table pool_gpu builds for texture handles traced to no slot (dmsh_build.py pool_arrays): a replaced table becomes a spare that a later rebuild writes again only once the
// submission that could still read it has completed (pool_table_take / pool_table_give), and an ID whose registration resolved to another texture type is passed over without its view being
// made again (treg_t.tt1): a 3D texture in a 2D table, after the submission end dropped its view as it does for a retired registration. A new registration of the ID forgets the type.
// The copy that patches a table comes from the spares too: the table draws already encoded hold keeps its bytes, and goes to the spares behind this submission's fence.
// A table of a uniform buffer's handles (htab_gpu) has a pool's form, so a shader's read marks it: on the GPU, through the chain's own dm_pool_mark, the mark lands in that table's bitmap and in no texture's.
// Last, the time that saves on a walk over 20,000 IDs of another type with dropped views, against looking each one up (under a quarter of it; it prints both).
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
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static void make_tex(NVNtexture *tex, pool_t *pool, int target, int depth, ptrdiff_t off) {
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, (NVNtextureTarget)target); h_nvnTextureBuilderSetSize3D(&tb, 4, 4, depth); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)pool, off);
    CHECK(h_nvnTextureInitialize(tex, &tb), "texture initialized");
}
// The table for 2D float textures at base 0, as a draw binding it gets it; the caller holds g_rmu
static id<MTLBuffer> table(void) { __unsafe_unretained id<MTLBuffer> b = nil; pool_gpu(0, MTLTextureType2D, 0, 0, &b); return b; }
static uint64_t entry(id<MTLBuffer> b, int i) { return ((const uint64_t *)((const uint8_t *)b.contents + DMSH_POOL_HEAD))[i]; }
// Out of date past the notes: the next use builds the table whole
static void stale(void) { for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf) g_psnap[k].reg = 0; }

int main(int argc, const char **argv) { @autoreleasepool {
    (void)argc; (void)argv;
    mtl_init();
    const size_t len = 1 << 20;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem = mem, .size = len, .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "pool initialized");
    NVNtexture a = {0}, vol = {0}, b = {0};
    make_tex(&a, &pool, NVN_TEXTURE_TARGET_2D, 1, 0); make_tex(&vol, &pool, NVN_TEXTURE_TARGET_3D, 4, 65536); make_tex(&b, &pool, NVN_TEXTURE_TARGET_2D, 1, 131072);
    mtl_reg_tex(1, (const tex_t *)&a, NULL); mtl_reg_tex(2, (const tex_t *)&vol, NULL); mtl_reg_tex(3, (const tex_t *)&b, NULL);
    for (int i = 4; i < 8; i++) mtl_reg_tex(i, NULL, NULL);   // room: a registration in it patches the table, one past it builds the table whole

    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    for (int i = 1; i <= 3; i++) tex_by_id(i);   // as a draw binding them does: a walk leaves a texture nothing resolved yet empty
    uint64_t made0 = g_pool_made, reused0 = g_pool_reused;
    id<MTLBuffer> t1 = table();
    CHECK(t1 && g_pool_made == made0 + 1, "the first table is a new buffer");
    CHECK(entry(t1, 1) && entry(t1, 1) == g_treg[1].view_rid && entry(t1, 3) == g_treg[3].view_rid, "2D IDs hold their textures");
    uint32_t n = *(const uint32_t *)t1.contents; uint64_t dummy = entry(t1, (int)n);
    CHECK(entry(t1, 2) == dummy, "the 3D ID holds the dummy");
    CHECK(g_treg[2].tt1 == MTLTextureType3D + 1 && g_treg[1].tt1 == MTLTextureType2D + 1, "each registration's type is known after its first lookup");
    treg_drop_views(&g_treg[2]);   // as the submission end does for a retired registration

    stale();
    uint64_t skip0 = g_stat_pool_tt_skip;
    id<MTLBuffer> t2 = table();
    CHECK(t2 && t2 != t1, "a rebuild while the replaced table may still be read writes another buffer");
    CHECK(g_stat_pool_tt_skip > skip0 && !g_treg[2].view, "the 3D ID is passed over without its view being made again");
    CHECK(entry(t2, 2) == dummy && entry(t2, 1) == entry(t1, 1), "the rebuilt table holds the dummy for it");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission that could read the first table completes");

    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    stale();
    id<MTLBuffer> t3 = table();
    CHECK(t3 == t1 && g_pool_reused == reused0 + 1, "the table replaced before the completed submission is written again");
    CHECK(entry(t3, 1) == g_treg[1].view_rid && entry(t3, 2) == dummy && entry(t3, (int)n) == dummy, "the reused table holds this rebuild's entries");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the submission that replaced the second table completes");

    // The copy that patches a table (draws already encoded read the old one): it comes from the spares, the old one stays as it is and goes to them
    mtl_sub_begin();
    mtl_reg_tex(5, (const tex_t *)&b, NULL);
    os_unfair_lock_lock(&g_rmu);
    uint64_t patched0 = g_stat_pool_patched, reused1 = g_pool_reused;
    id<MTLBuffer> t4 = table();
    CHECK(g_stat_pool_patched == patched0 + 1 && t4 != t3, "a registration patches a copy");
    CHECK(t4 == t2 && g_pool_reused == reused1 + 1, "the copy is a spare whose submission has completed");
    CHECK(entry(t3, 5) == dummy, "the table draws of this submission hold keeps its bytes");
    CHECK(entry(t4, 5) == g_treg[5].view_rid && entry(t4, 1) == g_treg[1].view_rid && entry(t4, (int)n) == dummy && *(const uint32_t *)t4.contents == n, "the copy holds the old entries and the new one");
    os_unfair_lock_unlock(&g_rmu);
    mtl_reg_tex(6, (const tex_t *)&b, NULL);
    os_unfair_lock_lock(&g_rmu);
    id<MTLBuffer> t5 = table();
    CHECK(t5 != t3 && t5 != t4, "a second patch takes neither table this submission holds");
    CHECK(entry(t4, 6) == dummy && entry(t3, 5) == dummy && entry(t5, 5) == entry(t4, 5) && entry(t5, 6) == g_treg[6].view_rid, "and leaves both as they were");
    os_unfair_lock_unlock(&g_rmu);

    mtl_reg_tex(2, (const tex_t *)&b, NULL);   // the ID now holds a 2D texture
    os_unfair_lock_lock(&g_rmu);
    CHECK(g_treg[2].tt1 == 0, "a new registration forgets the type");
    id<MTLBuffer> t6 = table();
    CHECK(t6 && t6 != t5 && entry(t6, 2) && entry(t6, 2) == g_treg[2].view_rid, "the ID is looked up again and holds its new texture");
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the last submission completes");

    // A spare is not handed out before its fence
    mtl_sub_begin();
    os_unfair_lock_lock(&g_rmu);
    size_t want = DMSH_POOL_HEAD + 8 * ((size_t)n + 1);
    for (int i = 0; i < g_npool_spare; i++) g_pool_spare[i].fence = atomic_load(&g_done) + 1;
    uint64_t new0 = g_pool_made; id<MTLBuffer> busy = pool_table_take(want);
    CHECK(g_pool_made == new0 + 1, "no spare is taken while a submission may still read it");
    defer_obj_release(busy);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "that submission completes");
    CHECK(!mtl_last_commit_error(), "no GPU error");

    {   // dmsh_build.py pool_arrays gives a table of handles the read it gives a pool, so the kernel takes POOL_MARK from there. With no bitmap in the table's head the mark writes to address 0, which an M5 Pro drops with no fault: the bitmap's bit is what tells them apart
        NSString *tool = [NSString stringWithContentsOfFile:@"../tools/dmsh_build.py" encoding:NSUTF8StringEncoding error:nil];
        NSRange at = [tool rangeOfString:@"POOL_MARK = \"\"\""], end = at.length ? [tool rangeOfString:@"\"\"\"" options:0 range:NSMakeRange(NSMaxRange(at), tool.length - NSMaxRange(at))] : at;
        CHECK(at.length && end.length, "the chain's mark is in dmsh_build.py");
        NSString *mark = [tool substringWithRange:NSMakeRange(NSMaxRange(at), end.location - NSMaxRange(at))];
        NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\nstruct spvPool_t { uint n; ulong seen; array<ulong, 1> e; };\n"
                         "kernel void main0(constant spvPool_t* t [[buffer(0)]], device uint* out [[buffer(1)]]) { out[0] = dm_pool_mark(t, min(2u, t->n)); }\n", mark];
        NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&error];
        id<MTLComputePipelineState> pso = lib ? [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&error] : nil;
        CHECK(pso, "the mark compiles"); if (!pso) fprintf(stderr, "%s\n", error.description.UTF8String);
        uint64_t *handles = (uint64_t *)(void *)(mem + 0x80000); handles[0] = 1; handles[1] = 3; handles[2] = 1; handles[3] = 3;   // texture IDs, in the low bits of each handle
        S.ubo[1][0].addr = (uint64_t)(uintptr_t)handles; S.ubo[1][0].size = 32;
        nvn_rbind hb = {.kind = NVN_RB_TEXH, .reg = 0, .off = 0, .dtt = MTLTextureType2D, .dpf = 0}; __unsafe_unretained id<MTLBuffer> ht = nil;
        mtl_sub_begin(); os_unfair_lock_lock(&g_rmu); htab_gpu(1, &hb, &ht); os_unfair_lock_unlock(&g_rmu);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the table's submission completes");
        CHECK(ht && *(const uint32_t *)ht.contents == 4, "a table of the four handles");
        id<MTLBuffer> out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; *(uint32_t *)out.contents = 0xdeadbeef;
        id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pso]; [ce setBuffer:ht offset:0 atIndex:0]; [ce setBuffer:out offset:0 atIndex:1];
        if (g_htab_seen) [ce useResource:g_htab_seen usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        printf("a read of a table of handles: status %ld, error %s, out %u\n", (long)cb.status, cb.error ? cb.error.localizedDescription.UTF8String : "none", *(uint32_t *)out.contents);
        CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error, "a read of a table of handles runs on the GPU");
        CHECK(*(uint32_t *)out.contents == 2, "and reads its element");
        CHECK(g_htab_seen && (*(const uint32_t *)g_htab_seen.contents & 1u << 2), "the mark is in the table's own bitmap");
        CHECK(!pool_seen(2) && !(g_seen_buf && (*(const uint32_t *)g_seen_buf.contents & 1u << 2)), "and not in the texture IDs': an element is no ID");
        memset(&S.ubo[1][0], 0, sizeof S.ubo[1][0]);
    }

    {   // What the type saves: a 2D-array table walked over 20,000 2D IDs whose views were dropped (a retired registration's go at every submission end), with the type cleared
        // (each ID looked up and its view made again, as before treg_t.tt1) and known (passed over). Ocarina of Time keeps about 12,000 retired descriptors
        enum { NB = 20000, REPS = 10 };
        for (int i = 10; i < 10 + NB; i++) mtl_reg_tex(i, (const tex_t *)&a, NULL);
        mtl_sub_begin();
        os_unfair_lock_lock(&g_rmu); for (int i = 10; i < 10 + NB; i++) tex_by_id(i); os_unfair_lock_unlock(&g_rmu);   // a walk leaves what nothing resolved empty, which costs nothing either way
        double ms[2] = {0, 0};
        for (int known = 0; known < 2; known++) for (int r = 0; r < REPS; r++) {
            os_unfair_lock_lock(&g_rmu);
            for (int i = 10; i < 10 + NB; i++) { if (!known) g_treg[i].tt1 = 0; else if (!g_treg[i].tt1) tex_by_id(i); treg_drop_views(&g_treg[i]); }
            for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf && g_psnap[k].dtt == MTLTextureType2DArray) g_psnap[k].reg = 0;   // out of date: the next use walks every ID
            uint64_t t0 = bp_now(); __unsafe_unretained id<MTLBuffer> tb = nil; pool_gpu(0, MTLTextureType2DArray, 0, 0, &tb); ms[known] += (bp_now() - t0) / 1e6 / REPS;
            os_unfair_lock_unlock(&g_rmu);
        }
        printf("a 2D-array table over %d 2D IDs with dropped views: %.3f ms a walk looking them up, %.3f ms passing over them\n", NB, ms[0], ms[1]);
        CHECK(ms[1] * 4 < ms[0], "passing over IDs of another type takes under a quarter of the time of looking them up");
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "the walks' submission completes");
        for (int i = 10; i < 10 + NB; i++) mtl_reg_tex(i, NULL, NULL);
    }
    for (int i = 1; i <= 7; i++) mtl_reg_tex(i, NULL, NULL);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    defer_drain(); munmap(mem, len);
    printf("t_pool_table: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
