// The offline sockets' Poll as the host serves it (hle_net.c dm_sk_poll), without a guest: descriptors the host never opened report POLLNVAL at once and are counted
// (Star Fox's libuv loop polls one beside a -1, and aborted when the call failed); a set with only negative descriptors, or none, waits out the timeout and returns 0.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_net.c"
#include <assert.h>

static int64_t slept = -1;
void dm_guest_sleep_ns(int64_t ns) { slept = ns; }

int main(void) {
    sk_pollfd p[3] = {{-1, 1, 7}, {2134729904, 1, 7}, {5, 4, 7}};
    assert(dm_sk_poll(p, 3, 33) == 2 && slept == -1);   // two invalid descriptors, no wait
    assert(p[0].revents == 0 && p[1].revents == SK_POLLNVAL && p[2].revents == SK_POLLNVAL);
    sk_pollfd q[2] = {{-1, 1, 7}, {-1, 4, 7}};
    assert(dm_sk_poll(q, 2, 33) == 0 && slept == 33000000 && q[0].revents == 0 && q[1].revents == 0);   // nothing to wait on: the timeout passes
    slept = -1; assert(dm_sk_poll(NULL, 0, -1) == 0 && slept == 100000000);   // an endless wait on nothing returns after 100 ms
    printf("t_net_poll: ok\n");
    return 0;
}
