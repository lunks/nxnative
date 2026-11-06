// The pipeline archive made offline and bundled with the app (DM_PSO_BUNDLE, pso_bundle_open / pso_bundle_covers in nvn_mtl.m), made here as build_ipa.sh makes it: tiny shader
// libraries and their reflection, a pair list, then dm/gfx/tools/pso_offline.py (metal-tt) for this Mac's GPU. Checks: the archive for this GPU opens and comes first among the files of
// bases without a vertex layout; with a matching stamp the prebuild builds only the pairs left to the device (a function with constants) and takes nothing from the compiler otherwise,
// and a draw of a covered pair takes its base from the bundle and captures nothing for the device's own archive;
// a stamp of another depth mode or other pairs runs the device's prebuild, which still takes the bundled bases it finds; an archive whose bases the probe does not find is not trusted;
// a folder without this GPU's file and a file that is no archive leave no archive; the compute shaders of the shader map have their pipelines in the archive (cs_pipeline, cps_new:
// nothing built), but a TensorOps library (<key>_bt in <key>_b's place) is never looked up, and a shader with function constants or not in the map is built as before; a task / mesh triple of
// mesh_pipelines.tsv comes from the archive (mtk_pso, nothing built), specialized for any colour format, and draws as one built on the device, a triple it does not hold is built, and one metal-tt
// refuses is left out alone.
// Without metal-tt (no Metal toolchain) it says so and passes
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, ...) do { uint64_t w = (want), g = (__VA_ARGS__); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL, with the SDK's compiler
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd %@ && xcrun -sdk macosx metal -c %@.metal -o %@.air && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
// One mesh threadgroup into a 4 x 4 target; its middle pixel
static void mesh_pixel(id<MTLRenderPipelineState> pipeline, MTLPixelFormat format, uint8_t out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1); pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [[g_dev newCommandQueue] commandBuffer]; id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    MTLSize one = MTLSizeMake(1, 1, 1);
    [enc setRenderPipelineState:pipeline]; [enc drawMeshThreadgroups:one threadsPerObjectThreadgroup:one threadsPerMeshThreadgroup:one]; [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("mesh draw completed", MTLCommandBufferStatusCompleted, cb.status);
    [target getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
}
static void reset_counts(void) { g_pre_done = g_pre_built = g_pre_archived = g_pre_skipped = g_pre_failed = 0; }

int main(void) { @autoreleasepool {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s/t_pso_bundle.XXXXXX", getenv("TMPDIR") ?: "/tmp"); if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *msl = [root stringByAppendingFormat:@"/msl/b-%s", DMSH_CHAIN], *tables = [root stringByAppendingPathComponent:@"tables"];
    NSString *bundle = [root stringByAppendingPathComponent:@"bundle"], *arch = [root stringByAppendingPathComponent:@"archive"];
    // A magic number alone cannot establish an archive: these are the known crash inputs, checked without calling Metal.
    NSString *bad = [root stringByAppendingPathComponent:@"bad.mtl4a"];
    const uint8_t magics[][4] = {{0xcf, 0xfa, 0xed, 0xfe}, {0xcb, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbf}};
    for (unsigned i = 0; i < sizeof magics / sizeof magics[0]; i++) {
        [[NSData dataWithBytes:magics[i] length:4] writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare magic refused", 0, arch_file_ok(bad));
        NSMutableData *header = [NSMutableData dataWithLength:32]; memcpy(header.mutableBytes, magics[i], 4); [header writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare header refused", 0, arch_file_ok(bad));
    }
    NSString *v = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n"
        "vertex V main0_z(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0.5, 1); return o; }\n";
    NSString *f = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(%d, %d, 0, 1); }\n";
    NSString *fc = @"#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\nfragment float4 main0() { return is_function_constant_defined(k) ? float4(1) : float4(0); }\n";
    NSString *k = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void main0(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = i * %d; }\n";
    NSString *kc = @"#include <metal_stdlib>\nusing namespace metal;\nconstant uint m [[function_constant(0)]];\n"
        "kernel void main0(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = is_function_constant_defined(m) ? m : i; }\n";
    NSString *meshHead = @"#include <metal_stdlib>\nusing namespace metal;\nstruct P { float x; };\nstruct V { float4 p [[position]]; };\nstruct W { float4 p [[position]]; float4 c [[user(locn0)]]; };\n";
    NSString *meshBody = @"  o.set_primitive_count(1); o.set_vertex(0, V{float4(-1, -1, %@, 1)}); o.set_index(0, 0); o.set_vertex(1, V{float4(3, -1, %@, 1)}); o.set_index(1, 1); o.set_vertex(2, V{float4(-1, 3, %@, 1)}); o.set_index(2, 2);\n";
    NSString *m1 = [meshHead stringByAppendingFormat:@"using T = mesh<V, void, 3, 1, topology::triangle>;\n[[mesh]] void main0(T o) {\n%@}\n[[mesh]] void main0_z(T o) {\n%@}\n", [meshBody stringByReplacingOccurrencesOfString:@"%@" withString:@"0"], [meshBody stringByReplacingOccurrencesOfString:@"%@" withString:@"0.5"]];
    NSString *m2 = [meshHead stringByAppendingFormat:@"using T = mesh<V, void, 3, 1, topology::triangle>;\n[[mesh]] void main0(T o, const object_data P &p [[payload]]) {\n%@}\n", [meshBody stringByReplacingOccurrencesOfString:@"%@" withString:@"p.x"]];
    NSString *t2 = [meshHead stringByAppendingString:@"[[object]] void main0(object_data P &p [[payload]], mesh_grid_properties g) { p.x = 0.25; g.set_threadgroups_per_grid(uint3(1, 1, 1)); }\n"];
    NSString *fu = [meshHead stringByAppendingString:@"fragment float4 main0(W i [[stage_in]]) { return i.c; }\n"];   // reads an output the mesh shaders do not write: metal-tt and the device refuse the pipeline
    [NSFileManager.defaultManager createDirectoryAtPath:msl withIntermediateDirectories:YES attributes:nil error:nil];
    if (lib(msl, @"m1_b", m1) || lib(msl, @"m2_b", m2) || lib(msl, @"t2_b", t2) || lib(msl, @"fu_b", fu) || lib(msl, @"v1_bp", v) || lib(msl, @"f1_b", [NSString stringWithFormat:f, 1, 0]) || lib(msl, @"f2_b", [NSString stringWithFormat:f, 0, 1]) || lib(msl, @"fc_b", fc) ||
        lib(msl, @"c1_b", [NSString stringWithFormat:k, 2]) || lib(msl, @"c1_bt", [NSString stringWithFormat:k, 3]) || lib(msl, @"c2_b", [NSString stringWithFormat:k, 4]) ||
        lib(msl, @"c3_b", [NSString stringWithFormat:k, 5]) || lib(msl, @"cc_b", kc)) {
        printf("t_pso_bundle: no Metal compiler (xcrun metal), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
    put([msl stringByAppendingPathComponent:@"v1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"pull\": {}, \"vertex_inputs\": []}", DMSH_CHAIN]);
    for (NSString *k in @[@"f1", @"f2", @"fc"]) put([msl stringByAppendingFormat:@"/%@_b.json", k], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\"}", DMSH_CHAIN]);
    for (NSString *c in @[@"c1", @"c2", @"c3", @"cc"]) put([msl stringByAppendingFormat:@"/%@_b.json", c], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"stage\": \"cs\", \"entry\": \"main0\"%@}",
                                                DMSH_CHAIN, [c isEqualToString:@"c1"] ? @", \"tensor\": {\"entry\": \"main0\", \"imma\": 1}" : @""]);
    put([msl stringByAppendingPathComponent:@"m1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"mesh_stage\": {\"kind\": \"ms\", \"threads\": 1, \"payload\": 0, \"task\": false}}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"m2_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\", \"mesh_stage\": {\"kind\": \"ms\", \"threads\": 1, \"payload\": 4, \"task\": true}}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"t2_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"route\": \"B\", \"bindings\": [], \"entry\": \"main0\", \"mesh_stage\": {\"kind\": \"ts\", \"threads\": 1, \"payload\": 4, \"task\": true}}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"fu_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\"}", DMSH_CHAIN]);
    put([tables stringByAppendingPathComponent:@"mesh_pipelines.tsv"], @"# triples\n-\tm1\tf1\nt2\tm2\tf1\n-\tm1\tfu\n");
    put([tables stringByAppendingPathComponent:@"shader_map.tsv"], @"package\tindex\tstage\tdm_ctl_sha16\tsource\ncs.bin\t0\tcompute\tc1\tx\ncs.bin\t1\tcompute\tc2\tx\ncs.bin\t2\tcompute\tcc\tx\nms.bin\t0\tmesh\tm1\tx\nms.bin\t1\tmesh\tm2\tx\nts.bin\t0\ttask\tt2\tx\n");
    setenv("DM_NVN_HMMA_TENSOR", "1", 1);   // the TensorOps libraries on any GPU, as on an Apple10 Mac by default
    put([tables stringByAppendingPathComponent:@"shader_pairs.tsv"], @"# pairs\nv1\tf1\nv1\tf2\nv1\tfc\n");
    setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1);
    setenv("DM_PSO_ARCHIVE", arch.UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init();
    NSString *gpu = g_dev.architecture.name;
    // the archive as build_ipa.sh makes it (the tool's own folder: this file is dm/gfx/tests/t_pso_bundle.m)
    NSString *tool = [[@(__FILE__) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../tools/pso_offline.py"];
    if (sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/shader_pairs.tsv --shader-map %@/shader_map.tsv --mesh %@/mesh_pipelines.tsv --out %@ --work %@/work > %@/pso_offline.log 2>&1",
            tool, gpu, msl, tables, tables, tables, bundle, root, root])) {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_offline.log"] encoding:NSUTF8StringEncoding error:nil];
        if ([log containsString:@"metal-tt is not available"]) { printf("t_pso_bundle: no metal-tt (xcodebuild -downloadComponent MetalToolchain), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
        fprintf(stderr, "pso_offline.py failed:\n%s\n", log.UTF8String); return 1;
    }
    EXPECT("bundle: this GPU's archive", 1, [NSFileManager.defaultManager fileExistsAtPath:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]]);
    NSData *archiveBytes = [NSData dataWithContentsOfFile:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]];
    EXPECT("archive guard: real offline archive accepted", 1, arch_file_ok([bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]));
    for (NSUInteger len = 32; len < archiveBytes.length; len = MIN(len * 2, archiveBytes.length)) {
        [[archiveBytes subdataWithRange:NSMakeRange(0, len)] writeToFile:bad atomically:YES];
        EXPECT("archive guard: truncated offline archive refused", 0, arch_file_ok(bad));
    }
    // Both standard universal wrappers must retain their slice bounds, including 64-bit offsets that cannot fit in the file.
    for (int wide = 0; wide < 2; wide++) {
        NSMutableData *wrapped = [NSMutableData dataWithLength:64];
        struct fat_header *fh = wrapped.mutableBytes; fh->magic = OSSwapHostToBigInt32(wide ? FAT_MAGIC_64 : FAT_MAGIC); fh->nfat_arch = OSSwapHostToBigInt32(1);
        if (wide) {
            struct fat_arch_64 *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt64(64); a->size = OSSwapHostToBigInt64(archiveBytes.length);
        } else {
            struct fat_arch *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt32(64); a->size = OSSwapHostToBigInt32((uint32_t)archiveBytes.length);
        }
        [wrapped appendData:archiveBytes]; [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal GPU image accepted", 1, arch_file_ok(bad));
        if (wide) ((struct fat_arch_64 *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->offset = UINT64_MAX;
        else ((struct fat_arch *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->size = UINT32_MAX;
        [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal slice outside file refused", 0, arch_file_ok(bad));
    }
    NSMutableData *broken = [archiveBytes mutableCopy];
    struct mach_header_64 *mh = broken.mutableBytes;
    EXPECT("archive fixture: one GPU image", MH_MAGIC_64, mh->magic);
    ((struct load_command *)((uint8_t *)broken.mutableBytes + sizeof *mh))->cmdsize = 0;
    [broken writeToFile:bad atomically:YES];
    EXPECT("archive guard: invalid load command refused", 0, arch_file_ok(bad));
    NSString *rest = [NSString stringWithContentsOfFile:[bundle stringByAppendingPathComponent:@"rest_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
    EXPECT("bundle: one pair left to the device", 1, [rest isEqualToString:@"v1\tfc\n"]);
    setenv("DM_PSO_BUNDLE", bundle.UTF8String, 1);
    shmap_load(); pso4_init();
    EXPECT("bundle: opened", 1, g_arch_bundle != nil);
    EXPECT("bundle: first among the files without a vertex layout", 1, g_arch4p.count && g_arch4p[0] == g_arch_bundle);
    // compute pipelines: the shader map's compute shaders come from the bundle; a TensorOps library is built, never looked up (a failed lookup of a TensorOps function makes
    // Metal refuse to build it); its <key>_b fallback, a shader with function constants and one the map does not have are not taken from the bundle unless it holds them
    @autoreleasepool {
        NvShader *c1 = shader_load(@"c1_b", 5, 0), *c2 = shader_load(@"c2_b", 5, 0), *c3 = shader_load(@"c3_b", 5, 0), *cc = shader_load(@"cc_b", 5, 0); NSError *err = nil;
        EXPECT("compute: shaders loaded, <key>_bt in <key>_b's place", 1, c1 && c2 && c3 && cc && c1->fn_b && !c2->fn_b);
        uint64_t a0 = g_cps_arch, b0 = g_cps_built; int n0 = g_arch_new;
        EXPECT("compute: a pipeline", 1, c2 && cs_pipeline(c2, &err) != nil);
        EXPECT("compute: from the bundle", a0 + 1, g_cps_arch);
        EXPECT("compute: nothing built", b0, g_cps_built);
        EXPECT("compute, TensorOps: a pipeline", 1, c1 && cs_pipeline(c1, &err) != nil);
        EXPECT("compute, TensorOps: built, not looked up", b0 + 1, g_cps_built);
        EXPECT("compute, TensorOps: not from the bundle", a0 + 1, g_cps_arch);
        dispatch_sync(cps_harvest_q(), ^{});
        EXPECT("compute, TensorOps: not captured into this device's archive (MTLDevice)", n0, g_arch_new);
        if (c1) atomic_store(&c1->fn_b_on, 1);   // Metal refused <key>_bt: <key>_b, which the bundle holds
        EXPECT("compute, <key>_b fallback: a pipeline", 1, c1 && cs_pipeline(c1, &err) != nil);
        EXPECT("compute, <key>_b fallback: from the bundle", a0 + 2, g_cps_arch);
        EXPECT("compute with constants: a pipeline", 1, cc && cs_pipeline(cc, &err) != nil);
        EXPECT("compute not in the map: a pipeline", 1, c3 && cs_pipeline(c3, &err) != nil);
        EXPECT("compute with constants, not in the map: built", b0 + 3, g_cps_built);
        EXPECT("compute with constants, not in the map: not from the bundle", a0 + 2, g_cps_arch);
        dispatch_sync(cps_harvest_q(), ^{});
        EXPECT("compute not in the map: captured into this device's archive, the one with constants not", n0 + 1, g_arch_new);
    }
    // task / mesh pipelines: the triples of mesh_pipelines.tsv come from the bundle at the first draw, whatever the colour state, and draw as one built on the device; another triple is built
    @autoreleasepool {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_offline.log"] encoding:NSUTF8StringEncoding error:nil];
        EXPECT("mesh: metal-tt's refusal left out one pipeline", 1, [log containsString:@"refused the task / mesh pipeline - m1_b fu_b: left out"]);
        NvShader *m1 = shader_load(@"m1_b", 4, 0), *m2 = shader_load(@"m2_b", 4, 0), *t2 = shader_load(@"t2_b", 3, 0), *f1 = shader_load(@"f1_b", 1, 0), *f2 = shader_load(@"f2_b", 1, 0);
        EXPECT("mesh: shaders loaded", 1, m1 && m2 && t2 && f1 && f2);
        state_reset(); S.chmask.mask = 15; memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; g_pass.dfmt = MTLPixelFormatDepth32Float; g_pass.sfmt = 0; g_pass.samples = 1;
        NvProg *raster = [NvProg new]; raster->sh[1] = f1;
        uint64_t a0 = atomic_load(&g_mtk_arch);
        g_mtk.ts = nil; g_mtk.ms = m1;
        id<MTLRenderPipelineState> plain = mtk_pso(raster);
        EXPECT("mesh: a pipeline", 1, plain != nil);
        EXPECT("mesh: from the bundle", a0 + 1, atomic_load(&g_mtk_arch));
        g_mtk.ts = t2; g_mtk.ms = m2;
        id<MTLRenderPipelineState> tasked = mtk_pso(raster);
        EXPECT("mesh with a task shader: a pipeline", 1, tasked != nil);
        EXPECT("mesh with a task shader: from the bundle", a0 + 2, atomic_load(&g_mtk_arch));
        g_pass.cfmt[0] = MTLPixelFormatBGRA8Unorm;   // another colour format, the same base
        id<MTLRenderPipelineState> other = mtk_pso(raster);
        EXPECT("mesh, another colour format: a pipeline", 1, other != nil && other != tasked);
        EXPECT("mesh, another colour format: from the bundle", a0 + 3, atomic_load(&g_mtk_arch));
        g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; g_pass.dfmt = 0; g_mtk.ts = nil; g_mtk.ms = m1; raster->sh[1] = f2;   // built on the device: its depth format is the target's
        id<MTLRenderPipelineState> built = mtk_pso(raster);   // not in the table: the lookup misses and it is built, so a failed lookup costs nothing
        EXPECT("mesh not in the table: a pipeline", 1, built != nil);
        EXPECT("mesh not in the table: not from the bundle", a0 + 3, atomic_load(&g_mtk_arch));
        if (plain && tasked && other && built) {
            uint8_t a[4], b[4], c[4], d[4];
            mesh_pixel(plain, MTLPixelFormatRGBA8Unorm, a); mesh_pixel(tasked, MTLPixelFormatRGBA8Unorm, b); mesh_pixel(other, MTLPixelFormatBGRA8Unorm, c); mesh_pixel(built, MTLPixelFormatRGBA8Unorm, d);
            EXPECT("mesh draw: red from the bundle's pipeline", 0xff0000ffu, (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]);
            EXPECT("mesh draw with a task shader: its payload reached the mesh stage", 1, !memcmp(a, b, 4));
            EXPECT("mesh draw, another colour format", 0x0000ffffu, (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3]);
            EXPECT("mesh draw built on the device", 0x00ff00ffu, (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
        }
        g_mtk.ts = g_mtk.ms = nil;
    }
    // A separate output forces one base per compiler invocation, then metal-pack merges both for this GPU.
    NSString *batched = [root stringByAppendingPathComponent:@"batched"];
    if (sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/shader_pairs.tsv --out %@ --work %@/batch-work --batch-size 1 > %@/pso_batched.log 2>&1",
            tool, gpu, msl, tables, batched, root, root])) {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_batched.log"] encoding:NSUTF8StringEncoding error:nil];
        fprintf(stderr, "batched pso_offline.py failed:\n%s\n", log.UTF8String); return 1;
    }
    NSString *batchPath = [batched stringByAppendingFormat:@"/%@.mtl4a", gpu];
    EXPECT("batched archive: container accepted", 1, arch_file_ok(batchPath));
    id<MTL4Archive> batchArchive = arch_file_ok(batchPath) ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:batchPath] error:nil] : nil;
    EXPECT("batched archive: opened", 1, batchArchive != nil);
    EXPECT("batched archive: first group's base", 1, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f1", 0) error:nil] != nil);
    EXPECT("batched archive: second group's base", 1, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f2", 0) error:nil] != nil);
    EXPECT("batched archive: absent depth entry stays absent", 0, [batchArchive newRenderPipelineStateWithDescriptor:pre_desc(@"v1", @"f1", 1) error:nil] != nil);
    EXPECT("batched archive: full coverage stamp preserved", 1, [[NSString stringWithContentsOfFile:[batched stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil]
            isEqualToString:[NSString stringWithContentsOfFile:[bundle stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil]]);
    // its stamp matches: only the pair with function constants is built here
    reset_counts();
    EXPECT("covered: one pair built on the device", 1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("covered: its base compiled", 1, g_pre_built);
    EXPECT("covered: nothing else", 0, g_pre_archived + g_pre_skipped + g_pre_failed);
    EXPECT("covered: again, nothing to do", -1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    // a draw of a covered pair (its pulling pipeline, as pso_desc_pull makes it) takes the base from the bundle: nothing compiled, nothing captured for the device's own archive
    @autoreleasepool {
        id<MTLLibrary> vl = lib_from_file([msl stringByAppendingPathComponent:@"v1_bp.metallib"], NULL), fl = lib_from_file([msl stringByAppendingPathComponent:@"f1_b.metallib"], NULL);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn4_note([vl newFunctionWithName:@"main0"], vl, @"main0", nil); d.fragmentFunction = fn4_note([fl newFunctionWithName:@"main0"], fl, @"main0", nil);
        d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uint64_t built0 = g_pb_built, arch0 = g_pb_archived; int new0 = g_arch_new; NSError *err = nil;
        EXPECT("covered draw: a pipeline", 1, pso_build(d, &err) != nil);
        EXPECT("covered draw: its base from the bundle", arch0 + 1, g_pb_archived);
        EXPECT("covered draw: none compiled", built0, g_pb_built);
        EXPECT("covered draw: nothing captured for the device's archive", new0, g_arch_new);
    }
    // the near = 0 depth mode: not what the archive was made for, so the device's prebuild runs, and its main0_z bases are not in the archive
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    reset_counts(); atomic_store(&g_depth_zero, 1);
    EXPECT("other depth mode: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other depth mode: every base compiled", 3, g_pre_built);
    atomic_store(&g_depth_zero, 0);
    // other pairs (a second list): the device's prebuild runs and takes the bundled bases it finds
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    put([tables stringByAppendingPathComponent:@"more.tsv"], @"v1\tf2\nv1\tfc\n"); setenv("DM_PSO_PAIRS", [tables stringByAppendingPathComponent:@"more.tsv"].UTF8String, 1);
    reset_counts();
    EXPECT("other pairs: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    // the base the "covered" step compiled is in a p<seq> file of the device's own by now (a pulling-only period's file is searched as soon as it is written), so that pair is found too
    EXPECT("other pairs: at least the two bundled bases found", 1, g_pre_archived >= 2);
    EXPECT("other pairs: every pair found or compiled", 3, g_pre_archived + g_pre_built);
    unsetenv("DM_PSO_PAIRS");
    // a matching stamp but an archive without the bases (here: one of the device's own files): the probe finds none, so it is not trusted
    @autoreleasepool {
        NSString *txt = [NSString stringWithContentsOfFile:[tables stringByAppendingPathComponent:@"shader_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
        NSString *own = [arch stringByAppendingPathComponent:[[[NSFileManager.defaultManager contentsOfDirectoryAtPath:arch error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'p' AND self ENDSWITH '.mtl4a'"]] firstObject]];
        id<MTL4Archive> real = g_arch_bundle; NSError *e = nil;
        g_arch_bundle = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:own] error:&e];
        EXPECT("archive guard: real serialized archive accepted", 1, arch_file_ok(own));
        EXPECT("probe: another archive opened", 1, g_arch_bundle != nil);
        EXPECT("probe: its bases are not this run's", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        g_arch_bundle = real;
        EXPECT("probe: the bundled archive holds them", 1, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        EXPECT("probe: not for the other depth mode", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 1), 1, rest));
        // Raw-pair sampling can hit only ineligible pairs: 0 of 0 lookups must not suppress the device prebuild.
        NSMutableString *sparse = [NSMutableString string];
        for (int i = 0; i < PRE_PROBE; i++) [sparse appendFormat:@"missing%d\tf1\n", i];
        [sparse appendString:@"v1\tf1\n"];
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(sparse, 0));
        EXPECT("probe: no eligible sample cannot establish coverage", 0, pso_bundle_covers(pre_parse(sparse), pre_stamp(sparse, 0), 0, nil));
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(txt, 0));
    }
    // a folder without this GPU's archive, and a file that is no archive (Metal crashed on one, and on a bare Mach-O header too): no archive, no crash
    @autoreleasepool {
        id<MTL4Archive> real = g_arch_bundle; NSString *other = [root stringByAppendingPathComponent:@"other"];
        put([other stringByAppendingPathComponent:@"applegpu_g99x.mtl4a"], @"x");
        NSMutableArray *list = [NSMutableArray array]; g_arch_bundle = nil;
        setenv("DM_PSO_BUNDLE", other.UTF8String, 1); pso_bundle_open(list);
        EXPECT("no file for this GPU: none opened", 0, list.count + (g_arch_bundle != nil));
        put([other stringByAppendingFormat:@"/%@.mtl4a", gpu], @"not an archive");   // Metal's own loadFromURL crashed on it: never handed to Metal
        pso_bundle_open(list);
        EXPECT("not an archive: none opened", 0, list.count + (g_arch_bundle != nil));
        EXPECT("not an archive: refused before Metal", 0, arch_file_ok([other stringByAppendingFormat:@"/%@.mtl4a", gpu]));
        setenv("DM_PSO_BUNDLE", "off", 1); pso_bundle_open(list);
        EXPECT("off: none opened", 0, list.count);
        g_arch_bundle = real;
    }
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_bundle: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
