#include "mygames/consumer_projection.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    mygames_consumer_projection *projection = NULL;
    uint64_t seq = UINT64_MAX, count = UINT64_MAX;
    char *end = NULL;
    unsigned long long expected_seq, expected_count;
    if (argc != 6) {
        fprintf(stderr,"usage: consumer_audit driver db namespace seq count\n");
        return 2;
    }
    expected_seq = strtoull(argv[4], &end, 10);
    if (!end || *end) return 2;
    expected_count = strtoull(argv[5], &end, 10);
    if (!end || *end) return 2;
    assert(mygames_consumer_projection_open(argv[1], argv[2], &projection) ==
           MYGAMES_CONSUMER_APPLIED);
    assert(mygames_consumer_projection_snapshot(
        projection, argv[3], "backend-projection-v1", &seq, &count) ==
           MYGAMES_CONSUMER_APPLIED);
    assert(seq == (uint64_t)expected_seq);
    assert(count == (uint64_t)expected_count);
    mygames_consumer_projection_close(projection);
    printf("mygames.flowie_consumer_projection_audit: PASS seq=%llu count=%llu\n",
        (unsigned long long)seq, (unsigned long long)count);
    return 0;
}
