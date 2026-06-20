#ifndef LIST_REPLAYER_H
#define LIST_REPLAYER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "prefetcher.h"

class CACHE;

// Replays externally generated prefetches at L2.
//
// Input format (PFETCH_LIST_PATH):
//   idx,prefetch_addr
//   0,0x1234...
//
// idx is the zero-based ordinal of an ROI L2 LOAD. The counter deliberately
// starts only after warmup_complete[cpu], matching the demand_idx produced by
// the residual/oracle logging pipeline.
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
    void load_table();

    CACHE* cache_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> table_;
    std::string path_;
    uint64_t counter_ = 0;
    uint64_t loaded_ = 0;
    uint64_t matched_ = 0;
    uint64_t emitted_ = 0;
    uint64_t rejected_lines_ = 0;
};

#endif // LIST_REPLAYER_H
