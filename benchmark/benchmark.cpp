#include <benchmark/benchmark.h>
#include <skiptracer.hpp>

using namespace skip;

struct Integer : public Trackable {
    std::uint32_t value{0};

    std::uint32_t todo(void) const {
        return value;
    }
};

static void BM_RawPointerAcces(benchmark::State& state) {
    Integer  val;
    Integer* ptr = &val;

    for (auto _ : state) {
        benchmark::DoNotOptimize(ptr->todo());
    }
}

// current performance of Tracker<T>::locate()
static void BM_TrackerAccess(benchmark::State& state) {
    Integer val;
    Tracker<Integer> t = val.tracker();

    for (auto _ : state) {
        benchmark::DoNotOptimize(t->todo());
    }
}

BENCHMARK(BM_RawPointerAcces)->Threads(1)->Threads(4)->Threads(8);
BENCHMARK(BM_TrackerAccess)->Threads(1)->Threads(4)->Threads(8);
BENCHMARK_MAIN();