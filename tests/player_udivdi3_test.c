/* @PRODUCTION_UDIVDI3@ */

#include <assert.h>
#include <stdio.h>

static uint64_t random_state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t random_value(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static void check(uint64_t numerator, uint64_t denominator)
{
    uint64_t expected = denominator ? numerator / denominator : UINT64_MAX;
    uint64_t observed = __udivdi3(numerator, denominator);
    if (expected != observed) {
        printf("udiv mismatch n=%016llx d=%016llx expected=%016llx observed=%016llx\n",
               (unsigned long long)numerator, (unsigned long long)denominator,
               (unsigned long long)expected, (unsigned long long)observed);
        assert(expected == observed);
    }
}

int main(void)
{
    static const uint64_t boundaries[] = {
        0, 1, 2, 3, 7, 8, 15, 16, 0x7fffffff, 0x80000000,
        0xffffffff, UINT64_C(0x100000000), UINT64_C(0x100000001),
        UINT64_C(0x7fffffffffffffff), UINT64_C(0x8000000000000000),
        UINT64_C(0x8000000000000001), UINT64_MAX - 1, UINT64_MAX
    };
    unsigned cases = 0;
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        for (unsigned j = 0; j < sizeof(boundaries) / sizeof(boundaries[0]); ++j) {
            check(boundaries[i], boundaries[j]);
            ++cases;
        }
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        uint64_t power = UINT64_C(1) << bit;
        for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
            check(boundaries[i], power);
            check(boundaries[i], power - 1);
            check(boundaries[i], power + 1);
            cases += 3;
        }
        check(power - 1, power);
        check(power, power);
        check(power + 1, power);
        cases += 3;
    }
    for (unsigned i = 0; i < 200000; ++i) {
        uint64_t numerator = random_value();
        uint64_t denominator = random_value() >> (i & 63u);
        check(numerator, denominator);
        ++cases;
    }
    printf("PASS production __udivdi3: %u boundary/random vectors, divide-by-zero=UINT64_MAX\n", cases);
    return 0;
}
