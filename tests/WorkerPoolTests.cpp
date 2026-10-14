#include "engine/WorkerPool.h"

#include <atomic>
#include <vector>

class WorkerPoolTests final : public juce::UnitTest
{
public:
    WorkerPoolTests() : juce::UnitTest ("WorkerPool", "LiveDAW") {}

    void runTest() override
    {
        for (int workers : { 0, 1, 3 })
        {
            beginTest ("Every index runs exactly once (" + juce::String (workers) + " workers)");

            livedaw::WorkerPool pool (workers);
            std::vector<std::atomic<int>> hits (257);

            for (int round = 0; round < 2000; ++round)
            {
                const int count = 1 + (round * 7) % 257;
                pool.parallelFor (count, [&] (int i) { hits[(size_t) i].fetch_add (1); });
            }

            int expected[257] = {};
            for (int round = 0; round < 2000; ++round)
                for (int i = 0; i < 1 + (round * 7) % 257; ++i)
                    ++expected[i];

            bool allMatch = true;
            for (size_t i = 0; i < hits.size(); ++i)
            {
                if (hits[i].load() != expected[i])
                    logMessage ("index " + juce::String ((int) i) + ": " + juce::String (hits[i].load())
                                + " != " + juce::String (expected[i]));
                allMatch = allMatch && hits[i].load() == expected[i];
            }

            expect (allMatch);
        }
    }
};

static WorkerPoolTests workerPoolTests;
