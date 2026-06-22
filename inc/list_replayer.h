#ifndef LIST_REPLAYER_H
#define LIST_REPLAYER_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "prefetcher.h"

class CACHE;

// Replays externally generated L2 prefetches at an event identity that survives
// timing-induced global reordering.
//
// PFETCH_LIST_PATH format:
//   pc,line,occ,prefetch_addr
//   4201648,1207676451699,0,0x464bc80edc40
//
// `occ` is the zero-based occurrence count of a (pc,line) demand pair in the
// no-prefetch oracle stream. The runtime ListReplayer maintains the same
// per-(pc,line) counter after warmup and looks up this PC-line-occ trigger.
//
// A global ROI-L2 ordinal cannot be used after prefetching: useful prefetches
// change memory timing and may reorder independent L2 callbacks. This keyed
// replay deliberately avoids assuming the no-prefetch global callback order is
// invariant under the intervention.
class ListReplayer : public Prefetcher
{
public:
    ListReplayer(std::string type, CACHE* cache);
    ~ListReplayer();

    void invoke_prefetcher(uint64_t pc, uint64_t address, uint8_t cache_hit,
                           uint8_t type, std::vector<uint64_t>& pref_addr);
    void dump_stats();
    void print_config();

private:
    struct TriggerKey {
        uint64_t pc = 0;
        uint64_t line = 0;
        uint64_t occ = 0;

        bool operator==(const TriggerKey& other) const
        {
            return pc == other.pc && line == other.line && occ == other.occ;
        }
    };

    struct TriggerKeyHash {
        size_t operator()(const TriggerKey& k) const
        {
            const size_t h1 = std::hash<uint64_t>()(k.pc);
            const size_t h2 = std::hash<uint64_t>()(k.line);
            const size_t h3 = std::hash<uint64_t>()(k.occ);
            return h1 ^ (h2 << 1) ^ (h3 << 7);
        }
    };

    struct PairKey {
        uint64_t pc = 0;
        uint64_t line = 0;

        bool operator==(const PairKey& other) const
        {
            return pc == other.pc && line == other.line;
        }
    };

    struct PairKeyHash {
        size_t operator()(const PairKey& k) const
        {
            const size_t h1 = std::hash<uint64_t>()(k.pc);
            const size_t h2 = std::hash<uint64_t>()(k.line);
            return h1 ^ (h2 << 1);
        }
    };

    void load_table();

    CACHE* cache_;
    std::unordered_map<TriggerKey, std::vector<uint64_t>, TriggerKeyHash> table_;
    std::unordered_map<PairKey, uint64_t, PairKeyHash> occurrences_;
    std::string path_;
    uint64_t counter_ = 0;
    uint64_t loaded_ = 0;
    uint64_t matched_ = 0;
    uint64_t emitted_ = 0;
    uint64_t rejected_lines_ = 0;
    bool stats_dumped_ = false;
};

#endif // LIST_REPLAYER_H
