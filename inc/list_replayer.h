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
// PFETCH_LIST_PATH format:
//   idx,prefetch_addr
//   0,0x1234...
//
// PFETCH_REF_PATH format:
//   idx,pc,line
//   0,4257721,24804268350
//
// idx is the zero-based ordinal of a post-warmup ROI L2 LOAD. The dense
// reference stream is checked at every runtime L2 LOAD before an entry can be
// emitted, so an early dynamic-stream shift cannot silently replay candidates
// at the wrong accesses.
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
    struct ReferenceSignature {
        uint64_t pc = 0;
        uint64_t line = 0;
    };

    void load_table();
    void load_reference();

    CACHE* cache_;
    std::unordered_map<uint64_t, std::vector<uint64_t>> table_;
    std::vector<ReferenceSignature> reference_;
    std::string path_;
    std::string reference_path_;
    uint64_t counter_ = 0;
    uint64_t loaded_ = 0;
    uint64_t matched_ = 0;
    uint64_t emitted_ = 0;
    uint64_t rejected_lines_ = 0;
    uint64_t reference_rejected_lines_ = 0;
    uint64_t signature_mismatches_ = 0;
    uint64_t reference_tail_accesses_ = 0;
    bool reference_enabled_ = false;
    bool stats_dumped_ = false;
};

#endif // LIST_REPLAYER_H
