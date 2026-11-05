// The address arbiter, svc 0x34 WaitForAddress and 0x35 SignalToAddress, through the real thread SVCs on one guest thread (mesosphere's KAddressArbiter and its svc checks):
// the four arbitration types, WaitIfEqual64's whole 64-bit compare (Horizon 18's type 3, which SDK 19 and later's nn::os::WaitForAddress(const long*, long) uses), the three
// signal types and the value each leaves, a 64-bit wait woken by a signal from another thread, and the Results for a misaligned address or a type the kernel does not have.
#include "../hle_thread.c"
#include <assert.h>
#include <unistd.h>

static int64_t wait_for(void *addr, int type, int64_t value, int64_t timeout) {
    uint64_t r[32] = {(uint64_t)addr, (uint64_t)type, (uint64_t)value, (uint64_t)timeout};
    svc_thread(0x34, r);
    return (int64_t)r[0];
}
static int64_t signal_to(void *addr, int type, int32_t value, int count) {
    uint64_t r[32] = {(uint64_t)addr, (uint64_t)type, (uint64_t)(uint32_t)value, (uint64_t)(uint32_t)count};
    svc_thread(0x35, r);
    return (int64_t)r[0];
}

static _Alignas(8) uint64_t word64;
static void *signaller(void *unused) {   // signals once the main thread waits on word64 (wait kind 3 on its address)
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&G);
        int waiting = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == (uint64_t)&word64) waiting = 1;
        pthread_mutex_unlock(&G);
        if (waiting) break;
        usleep(100);
    }
    atomic_store((_Atomic uint64_t *)&word64, 0x200000007ull);   // the value changes, then the waiter is woken, as a user-mode release does
    assert(signal_to(&word64, 0, 0, 1) == 0);
    return NULL;
}

int main(void) {
    alarm(20);
    svc_thread_main_init();
    static _Alignas(8) int32_t w[4];

    w[0] = 5;
    assert(wait_for(&w[0], 0, 10, 0) == R_TIMEOUT && w[0] == 5);    // WaitIfLessThan: 5 < 10, no time to wait
    assert(wait_for(&w[0], 0, 5, 0) == R_BAD_STATE);
    assert(wait_for(&w[0], 1, 10, 0) == R_TIMEOUT && w[0] == 4);    // DecrementAndWaitIfLessThan decrements before the timeout
    assert(wait_for(&w[0], 1, 4, 0) == R_BAD_STATE && w[0] == 4);   // and leaves the value when the test fails
    assert(wait_for(&w[0], 2, 4, 0) == R_TIMEOUT);                  // WaitIfEqual
    assert(wait_for(&w[0], 2, 5, 0) == R_BAD_STATE);
    puts("PASS: WaitIfLessThan, DecrementAndWaitIfLessThan and WaitIfEqual");

    word64 = 0x100000005ull;
    assert(wait_for(&word64, 3, 0x100000005ll, 0) == R_TIMEOUT);   // WaitIfEqual64: equal
    assert(wait_for(&word64, 3, 0x5, 0) == R_BAD_STATE);           // the low words equal, the high ones not
    assert(wait_for(&word64, 3, 0x200000005ll, 0) == R_BAD_STATE);
    assert(wait_for(&word64, 3, 0x100000005ll, 1000) == R_TIMEOUT);
    pthread_t s; assert(!pthread_create(&s, NULL, signaller, NULL));
    assert(wait_for(&word64, 3, 0x100000005ll, -1) == 0);         // woken by SignalToAddress
    assert(!pthread_join(s, NULL));
    puts("PASS: WaitIfEqual64 compares all 64 bits and is woken by a signal");

    w[1] = 7;
    assert(signal_to(&w[1], 0, 0, 1) == 0 && w[1] == 7);           // Signal: the value stays
    assert(signal_to(&w[1], 1, 7, 1) == 0 && w[1] == 8);           // SignalAndIncrementIfEqual
    assert(signal_to(&w[1], 1, 7, 1) == R_BAD_STATE && w[1] == 8);
    assert(signal_to(&w[1], 2, 8, 1) == 0 && w[1] == 9);           // SignalAndModifyByWaitingCountIfEqual with no waiter: + 1
    assert(signal_to(&w[1], 2, 8, -1) == R_BAD_STATE && w[1] == 9);
    puts("PASS: Signal, SignalAndIncrementIfEqual and SignalAndModifyByWaitingCountIfEqual");

    uint8_t *b = (uint8_t *)w;
    assert(wait_for(b + 2, 2, 0, 0) == R_BAD_ADDR);                 // 4-byte types: 4-byte aligned
    assert(wait_for(&w[1], 3, 0, 0) == R_BAD_ADDR);                 // WaitIfEqual64: 8-byte aligned (w[1] is at +4)
    assert(wait_for(&w[0], 4, 0, 0) == R_BAD_ENUM);
    assert(wait_for(&w[0], -1, 0, 0) == R_BAD_ENUM);
    assert(signal_to(b + 1, 0, 0, 1) == R_BAD_ADDR);
    assert(signal_to(&w[0], 3, 0, 1) == R_BAD_ENUM);                // no 64-bit signal type
    puts("PASS: a misaligned address is InvalidAddress, an unknown type InvalidEnumValue");
    return 0;
}
