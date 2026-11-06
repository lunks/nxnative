// The fence of a deferred release (nvn_mtl.m defer_obj_release: g_fence + 1, read by whichever thread frees). A texture the game finalizes on its own thread is released behind the submission the
// submit thread is encoding, and g_fence is stepped by that thread's mtl_sub_commit: the release is a race against the commit, and must come out as this submission's fence or a later one, never an
// earlier one, or the object goes while the GPU still runs the submission that used it. Here a thread releases an object the submit thread has just "used" in its open submission, while
// the submit thread commits; the object must not be freed before the fence of that submission completes
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static _Atomic uint64_t g_freed, g_early;   // objects freed; those freed before the submission that used them had completed
@interface Probe : NSObject { @public uint64_t use_fence; } @end
@implementation Probe
- (void)dealloc { atomic_fetch_add(&g_freed, 1); if (atomic_load(&g_done) < use_fence) atomic_fetch_add(&g_early, 1); }
@end

static _Atomic(void *) g_slot;       // the object handed to the releasing thread (a retained Probe), NULL when empty
static _Atomic int g_stop;
static _Atomic uint64_t g_released;
static void *releaser(void *arg) {   // the game's thread: nvnTextureFinalize reaching defer_obj_release
    (void)arg;
    while (!atomic_load(&g_stop)) {
        void *v = atomic_exchange(&g_slot, NULL);
        if (!v) continue;
        Probe *p = (Probe *)CFBridgingRelease(v);
        defer_obj_release(p);
        atomic_fetch_add(&g_released, 1);
    }
    return NULL;
}
int main(int argc, char **argv) { @autoreleasepool {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    mtl_init();
    pthread_t th; pthread_create(&th, NULL, releaser, NULL);
    for (int i = 0; i < n; i++) {
        mtl_sub_begin();                                           // drains what is due
        Probe *p = [Probe new]; p->use_fence = atomic_load(&g_fence) + 1;   // the submission being encoded uses it
        uint64_t before = atomic_load(&g_released);
        while (atomic_load(&g_slot)) {}                            // the previous hand-over was taken
        atomic_store(&g_slot, (void *)CFBridgingRetain(p));
        p = nil;
        if (i % 3 == 0) while (atomic_load(&g_released) == before) {}   // sometimes the release comes first, else it races the commit
        mtl_sub_commit();
    }
    while (atomic_load(&g_slot)) {}
    atomic_store(&g_stop, 1); pthread_join(th, NULL);
    for (int i = 0; i < 50 && atomic_load(&g_freed) < (uint64_t)n; i++) { mtl_sub_begin(); mtl_sub_commit(); mtl_drain(); defer_drain(); }
    CHECK(atomic_load(&g_released) == (uint64_t)n, "every object was handed to the releasing thread");
    CHECK(atomic_load(&g_freed) == (uint64_t)n, "every released object was freed once its fence passed");
    CHECK(atomic_load(&g_early) == 0, "none was freed before the submission that used it had completed");
    printf("t_defer_fence: %d checks, %d failed (%llu objects, %llu freed early)\n", checks, failed, (unsigned long long)atomic_load(&g_freed), (unsigned long long)atomic_load(&g_early));
    return failed != 0;
} }
