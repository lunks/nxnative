// t_present_hdr's vi side (hle_vi.c dm_vi_hdr_out): a strong definition in its own object overrides nvn_mtl.m's weak default. The test sets what it answers
int g_test_hdr = -1, g_test_hdr_pq = 1, g_test_hdr_2020 = 1;
float g_test_hdr_paper = 203;
int dm_vi_hdr_out(int f16, float *to_edr, int *pq, int *bt2020) {
    (void)f16;
    *pq = g_test_hdr_pq; *bt2020 = g_test_hdr_2020; *to_edr = (g_test_hdr_pq ? 10000.0f : 80.0f) / g_test_hdr_paper;
    return g_test_hdr;
}
// The front end's side (front_mac.m): the screen's headroom the test sets, and the layer mode the present last asked for
float g_test_edr = 0; int g_test_layer = -1;
void front_screen_edr(float *potential, float *current) { *potential = *current = g_test_edr; }
void front_layer_hdr(int mode) { g_test_layer = mode; }
