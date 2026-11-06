// A translated load at an address the shader read from memory (GlobalToStorage.TryGenerateRawStorageOp), run on the GPU against the host's own pool table (nvn_mtl.m gmem_map) with
// thousands of pools: each probe must read its pool's word, whether the lookup takes the chunk hint (a large pool) or the binary search (small pools), and an address in no pool reads 0. The shader is test_dmsh_ampere_mem.py's RawGlobal one: it loads the address from words 0 and 1 of storage slot 0 and stores what it read at word 2.
// Then the last entry a lookup keeps (GlobalToStorage.TryGenerateRawStorageOp), with many lookups an invocation: lanes that walk a list in different orders, some skipping steps, so that they hit and miss apart, read what each
// address holds; stores through it land at their addresses and set the log bit of each pool they reach, and of no other.
// Usage: t_gmem_raw <metallib> <walk metallib> <store metallib>
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label, c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 4) { fprintf(stderr, "usage: t_gmem_raw <metallib> <walk metallib> <store metallib>\n"); return 2; }
    mtl_init();
    enum { MANY = 5000, PG = 16384 };
    const uint64_t MB = 1ull << 20;
    uint8_t *many = mmap(NULL, (size_t)MANY * 2 * PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0), *large = mmap(NULL, 64 * MB, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (many == MAP_FAILED || large == MAP_FAILED) abort();
    for (int i = 0; i < MANY; i++) { uint8_t *p = many + (size_t)i * 2 * PG; mtl_pool_add(p, PG); mtl_pool_addressed(p, PG); uint32_t v = 0x10000u + (unsigned)i; memcpy(p + (size_t)(i * 8 % PG), &v, 4); }
    mtl_pool_add(large, 64 * MB); mtl_pool_addressed(large, 64 * MB); { uint32_t v = 0xb16b00b5u; memcpy(large + 40 * MB + 12, &v, 4); }
    mtl_sub_begin(); id<MTLBuffer> map = gmem_map(); CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error]; if (!lib) { fprintf(stderr, "library: %s\n", error.description.UTF8String); return 1; }
    id<MTLComputePipelineState> pso = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&error]; if (!pso) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }
    id<MTLBuffer> slot = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared], args = [g_dev newBufferWithLength:24 options:MTLResourceStorageModeShared], sites = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    memset(sites.contents, 0, 256);   // the bind-time pointer site entries (DMSH_KIND_GPTR): all empty, so every load searches the table
    uint64_t *a = args.contents; a[0] = slot.gpuAddress; a[1] = map.gpuAddress; a[2] = sites.gpuAddress;   // the argument buffer: storage slot 0 [[id(0)]], the pool table [[id(1)]], the site entries [[id(2)]]
    uint32_t (^probe)(uint64_t) = ^uint32_t(uint64_t addr) {
        uint32_t *w = slot.contents; memcpy(w, &addr, 8); w[2] = 0xdeadbeefu;
        id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pso]; [ce setBuffer:args offset:0 atIndex:0];
        [ce useResource:slot usage:MTLResourceUsageRead | MTLResourceUsageWrite]; [ce useResource:map usage:MTLResourceUsageRead]; [ce useResource:sites usage:MTLResourceUsageRead];
        [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK("the dispatch completes", cb.status == MTLCommandBufferStatusCompleted);
        return w[2];
    };
    int hits = 0, n = 0;
    for (int i = 0; i < MANY; i += 97) { n++; uint64_t p = (uint64_t)(uintptr_t)(many + (size_t)i * 2 * PG); hits += probe(p + (uint64_t)(i * 8 % PG)) == 0x10000u + (unsigned)i; }
    { int i = MANY - 1; n++; uint64_t p = (uint64_t)(uintptr_t)(many + (size_t)i * 2 * PG); hits += probe(p + (uint64_t)(i * 8 % PG)) == 0x10000u + (unsigned)i; }
    CHECK("small pools, the search: each probe reads its pool", hits == n);
    CHECK("a large pool, the hint", probe((uint64_t)(uintptr_t)large + 40 * MB + 12) == 0xb16b00b5u);
    CHECK("an address in no pool reads 0", probe((uint64_t)(uintptr_t)many + PG + 8) == 0);
    // The range cache: each lane walks the list its own way through one cache
    enum { LANES = 64, STEPS = 48 };
    uint64_t p0 = (uint64_t)(uintptr_t)(many + (size_t)10 * 2 * PG), p1 = (uint64_t)(uintptr_t)(many + (size_t)11 * 2 * PG), hole = p0 + PG + 8, big = (uint64_t)(uintptr_t)large;
    for (int k = 0; k < 256; k++) { uint32_t a = 0xa0000 + (unsigned)k, b = 0xb0000 + (unsigned)k, c = 0xc0000 + (unsigned)k; memcpy((void *)(p0 + 4096 + 4 * (unsigned)k), &a, 4); memcpy((void *)(p1 + 4096 + 4 * (unsigned)k), &b, 4); memcpy((void *)(big + 32 * MB + 4 * (unsigned)k), &c, 4); }
    id<MTLBuffer> list = [g_dev newBufferWithLength:STEPS * 8 options:MTLResourceStorageModeShared], outs = [g_dev newBufferWithLength:LANES * STEPS * 4 options:MTLResourceStorageModeShared], cnt = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    uint64_t *L = list.contents; *(uint32_t *)cnt.contents = STEPS;
    for (int k = 0; k < STEPS; k++) { int w = k % 12, off = 4 * k;   // runs in one range, a hole in no pool, ranges that follow one another, a return to the first
        L[k] = w < 4 ? p0 + 4096 + (uint64_t)off : w == 4 ? hole : w < 7 ? p0 + 4096 + (uint64_t)off : w < 9 ? p1 + 4096 + (uint64_t)off : w == 9 ? big + 32 * MB + (uint64_t)off : w == 10 ? 8 : p1 + 4096 + (uint64_t)off; }
    id<MTLLibrary> wlib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&error]; if (!wlib) { fprintf(stderr, "library: %s\n", error.description.UTF8String); return 1; }
    id<MTLComputePipelineState> wpso = [g_dev newComputePipelineStateWithFunction:[wlib newFunctionWithName:@"walk"] error:&error]; if (!wpso) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }
    { memset(outs.contents, 0xee, outs.length);
      id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:wpso];
      [ce setBuffer:map offset:0 atIndex:0]; [ce setBuffer:list offset:0 atIndex:1]; [ce setBuffer:outs offset:0 atIndex:2]; [ce setBuffer:cnt offset:0 atIndex:3];
      [ce dispatchThreads:MTLSizeMake(LANES, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
      CHECK("the walk completes", cb.status == MTLCommandBufferStatusCompleted);
      const uint32_t *o = outs.contents; int wrong = 0, ran = 0, skipped = 0;
      for (unsigned lane = 0; lane < LANES; lane++) for (unsigned k = 0; k < STEPS; k++) {
          uint32_t got = o[lane * STEPS + k];
          if ((lane & 3) == 2 && k % 3 == 1) { skipped += got == 0xeeeeeeeeu; continue; }
          uint64_t a = L[lane & 1 ? STEPS - 1 - k : k]; uint32_t want = 0;
          if (a != hole && a != 8) memcpy(&want, (const void *)a, 4);
          ran++; wrong += got != want; }
      CHECK("each lane read what each address holds, with hits and misses apart", !wrong && ran > 2000 && skipped > 100);
      printf("t_gmem_raw: %d lookups in a walk, %d wrong\n", ran, wrong); }
    // The stores: lanes in both orders over two pools, a hole and a large pool; each address written, the log's bits those pools' entries only
    { enum { SN = 12 }; id<MTLLibrary> slib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error]; if (!slib) { fprintf(stderr, "library: %s\n", error.description.UTF8String); return 1; }
      id<MTLComputePipelineState> spso = [g_dev newComputePipelineStateWithFunction:[slib newFunctionWithName:@"walk"] error:&error]; if (!spso) { fprintf(stderr, "pipeline: %s\n", error.description.UTF8String); return 1; }
      id<MTLBuffer> log = [g_dev newBufferWithLength:GML_BYTES options:MTLResourceStorageModeShared]; memset(log.contents, 0, log.length);
      const uint64_t base[3] = {p0 + 8192, p1 + 8192, big + 40 * MB + 4096}; uint64_t S[SN];
      for (int k = 0; k < SN; k++) { S[k] = k % 4 == 3 ? hole : base[k % 4] + (uint64_t)(k / 4) * 1024; L[k] = S[k]; }
      *(uint32_t *)cnt.contents = SN;
      id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder]; [ce setComputePipelineState:spso];
      [ce setBuffer:map offset:0 atIndex:0]; [ce setBuffer:log offset:0 atIndex:1]; [ce setBuffer:list offset:0 atIndex:2]; [ce setBuffer:cnt offset:0 atIndex:3];
      [ce dispatchThreads:MTLSizeMake(LANES, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
      CHECK("the stores complete", cb.status == MTLCommandBufferStatusCompleted);
      int wrong = 0; for (unsigned lane = 0; lane < LANES; lane++) for (unsigned k = 0; k < SN; k++) if (S[k] != hole) { uint32_t got; memcpy(&got, (const void *)(S[k] + 4 * lane), 4); wrong += got != 0x5000 + lane * 64 + k; }
      CHECK("each store landed at its address", !wrong);
      const uint64_t *tab = map.contents; const uint32_t *bits = log.contents; unsigned set = 0, want = 0, entries = DMSH_GMEM_MAP_ENTRIES;
      for (unsigned e = 1; e < entries; e++) { int on = bits[e >> 5] >> (e & 31) & 1; uint64_t lo = tab[e * (DMSH_GMEM_MAP_ENTRY / 8)], hi = tab[e * (DMSH_GMEM_MAP_ENTRY / 8) + 1]; int hit = 0; for (int k = 0; k < 3; k++) hit |= base[k] >= lo && base[k] < hi;
          set += (unsigned)on; want += (unsigned)hit; if (on != hit) wrong++; }
      CHECK("the log has each pool's bit and no other", !wrong && want == 3 && set == 3); }
    // A pool made after a raw shader's arguments were encoded: at commit the word is pointed at a table that has it, and it reads its word, where the older table reads 0
    { mtl_sub_begin(); id<MTLBuffer> before = gmem_map(); a[1] = before.gpuAddress; gmem_slot_note(&a[1], NULL);
      uint8_t *late = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (late == MAP_FAILED) abort(); uint32_t v = 0x1a7e0001u; memcpy(late + 64, &v, 4); mtl_pool_add(late, PG); mtl_pool_addressed(late, PG);
      uint64_t words, older, behind, words0, older0, behind0; mtl_gmem_words(&words0, &older0, &behind0);
      CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
      mtl_gmem_words(&words, &older, &behind);
      CHECK("the word holds the table of the pools as they are", a[1] == g_gmem_buf.gpuAddress && a[1] != before.gpuAddress);
      CHECK("counted as behind the pools", words == words0 + 1 && behind == behind0 + 1 && older == older0);
      CHECK("the late pool reads its word", probe((uint64_t)(uintptr_t)late + 64) == v);
      uint64_t now_table = a[1]; a[1] = before.gpuAddress; CHECK("the table encoded with reads 0 there", probe((uint64_t)(uintptr_t)late + 64) == 0); a[1] = now_table; }
    // A word holding an older table than the current one: a later encode of the submission built a newer one
    { mtl_sub_begin(); uint64_t old_word = gmem_map().gpuAddress, keep = old_word; gmem_slot_note(&old_word, NULL);
      uint8_t *late = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (late == MAP_FAILED) abort(); mtl_pool_add(late, PG); mtl_pool_addressed(late, PG);
      uint64_t newer = gmem_map().gpuAddress, words0, older0, behind0, words, older, behind; mtl_gmem_words(&words0, &older0, &behind0);
      CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); mtl_gmem_words(&words, &older, &behind);
      CHECK("an older table's word follows the newer one", old_word == newer && newer != keep && older == older0 + 1); }
    // A raw writer's log goes with the table: its bits are that table's entries
    { mtl_sub_begin(); id<MTLBuffer> before = gmem_map(); uint64_t tw = before.gpuAddress; gml_take(before); NvGmlLog *log = g_gml_last; gmem_slot_note(&tw, (__bridge void *)log);
      uint8_t *late = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (late == MAP_FAILED) abort(); mtl_pool_add(late, PG); mtl_pool_addressed(late, PG);
      CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull));
      CHECK("the log's table is the word's", log->map == g_gmem_buf && tw == g_gmem_buf.gpuAddress && tw != before.gpuAddress); }
    // An unchanged table leaves the word and the counts alone
    { mtl_sub_begin(); uint64_t w = gmem_map().gpuAddress, w0 = w, n0, o0, b0, n1, o1, b1; gmem_slot_note(&w, NULL); mtl_gmem_words(&n0, &o0, &b0);
      CHECK("submission completes", mtl_wait(mtl_sub_commit(), 5000000000ull)); mtl_gmem_words(&n1, &o1, &b1);
      CHECK("nothing to do", w == w0 && n1 == n0 + 1 && o1 == o0 && b1 == b0); }
    printf("t_gmem_raw: %d checks, %d failed (%d probes)\n", checks, failed, n + 2);
    return failed != 0;
}}
