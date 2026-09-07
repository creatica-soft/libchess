// c++ -std=c++20 -w -O3 -I /Users/ap/libchess -L /Users/ap/libchess -Wl,-lchess,-rpath,/Users/ap/libchess bench_map.cpp -o bench_map
//
// Isolates the one operation the collector actually spends its time in, because the self-play
// A/B could not: two engines playing from the same position diverge into different games, so
// they perform different numbers of collections over different tree sizes and the totals are
// not comparable.
//
// This replays gc()'s inner loop against both table types with identical work: fill the table,
// then walk it and erase a fixed fraction, exactly as the sweep does -- including the pointer
// chase into the node to read its generation, which is where much of the cache cost lives.

#include "nnue/bitboard.h"
#include "creatica_search.hpp"
#include <chrono>
#include <cstdio>
#include <random>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

template <typename Map>
static void run(const char * name, size_t N, int rounds, double kill_fraction) {
    // Real nodes, so the sweep's read of node->generation is a genuine pointer chase into
    // scattered memory rather than a prediction-friendly array scan.
    std::vector<MCTSNode *> nodes;
    nodes.reserve(N);
    for (size_t i = 0; i < N; ++i) nodes.push_back(new MCTSNode());

    std::mt19937_64 rng(12345);          // same keys for both tables
    std::vector<uint64_t> keys(N);
    for (size_t i = 0; i < N; ++i) keys[i] = rng();

    Map tree;
    double fill_ms = 0, walk_ms = 0;
    size_t erased_total = 0;

    for (int r = 0; r < rounds; ++r) {
        // Refill to N, as the search does between collections.
        auto t0 = clk::now();
        for (size_t i = 0; i < N; ++i) {
            if (tree.find(keys[i]) == tree.end()) tree.emplace(keys[i], nodes[i]);
        }
        fill_ms += ms_since(t0);

        // Mark: give the survivors the current generation.
        const int gen = r + 1;
        const size_t survivors = (size_t)(N * (1.0 - kill_fraction));
        for (size_t i = 0; i < survivors; ++i)
            nodes[i]->generation.store(gen, std::memory_order_relaxed);

        // Sweep, exactly as gc() does it.
        t0 = clk::now();
        std::vector<MCTSNode *> dead;
        size_t erased = 0;
        for (auto it = tree.begin(); it != tree.end();) {
            MCTSNode * n = it->second;
            if (n->generation.load(std::memory_order_relaxed) < gen) {
                it = tree.erase(it);
                dead.push_back(n);
                ++erased;
            } else ++it;
        }
        walk_ms += ms_since(t0);
        erased_total += erased;
    }

    printf("  %-16s fill %7.0f ms   sweep %7.0f ms   (%zu erased over %d rounds)\n",
           name, fill_ms, walk_ms, erased_total, rounds);

    for (MCTSNode * n : nodes) delete n;
}

// The pre-change table, so both are measured in this same process and machine state.
struct NoOpHash { std::size_t operator()(uint64_t k) const noexcept { return k; } };
struct MapAdapter {
    std::unordered_map<uint64_t, MCTSNode *, NoOpHash> m;
    auto begin() { return m.begin(); }
    auto end()   { return m.end(); }
    auto find(uint64_t k) { return m.find(k); }
    auto erase(decltype(m.begin()) it) { return m.erase(it); }
    void emplace(uint64_t k, MCTSNode * v) { m.emplace(k, v); }
    size_t size() const { return m.size(); }
};

int main(int argc, char ** argv) {
    const size_t N      = (argc > 1) ? strtoull(argv[1], nullptr, 10) : 2000000;
    const int    rounds = (argc > 2) ? atoi(argv[2]) : 6;
    const double kill   = (argc > 3) ? atof(argv[3]) : 0.80;
    printf("bench_map: %zu entries, %d rounds, killing %.0f%% each round\n", N, rounds, kill * 100);
    run<MapAdapter>("unordered_map", N, rounds, kill);
    run<NodeMap>("NodeMap(flat)", N, rounds, kill);
    return 0;
}
