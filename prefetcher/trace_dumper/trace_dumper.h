#ifndef TRACE_DUMPER_H
#define TRACE_DUMPER_H

#include "address.h"
#include "cache.h"
#include "modules.h"
#include <cstdio>
#include <cstdint>

class trace_dumper : public champsim::modules::prefetcher
{
public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  void prefetcher_final_stats();
  uint32_t prefetcher_cache_operate(champsim::address addr,
                                    champsim::address ip,
                                    bool cache_hit,
                                    bool useful_prefetch,
                                    access_type type,
                                    uint32_t metadata_in);

private:
  std::FILE* fh_ = nullptr;
  uint64_t   counter_ = 0;
};

#endif
