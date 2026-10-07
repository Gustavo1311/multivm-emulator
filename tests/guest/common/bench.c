/* Benchmark: repete os testes de CPU de test.c (compilado com -Dtest_main=test_main_once). */
int test_main_once(void);
#ifndef BENCH_N
#define BENCH_N 200
#endif
int test_main(void)
{
    int r = 0;
    for (int i = 0; i < BENCH_N; i++)
        r |= test_main_once();
    return r;
}
