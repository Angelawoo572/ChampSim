#include "list_replayer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "cache.h"
#include "champsim.h"

using namespace std;

namespace {
vector<ListReplayer*> list_replayer_instances;
bool list_replayer_atexit_registered = false;

string trim_copy(string s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

void dump_all_list_replayers()
{
    for (auto* p : list_replayer_instances) {
        if (p) p->dump_stats();
    }
}
} // namespace

ListReplayer::ListReplayer(string type, CACHE* cache)
    : Prefetcher(type), cache_(cache)
{
    load_table();
    list_replayer_instances.push_back(this);
    if (!list_replayer_atexit_registered) {
        std::atexit(dump_all_list_replayers);
        list_replayer_atexit_registered = true;
    }
}

ListReplayer::~ListReplayer() = default;

void ListReplayer::load_table()
{
    const char* env = std::getenv("PFETCH_LIST_PATH");
    path_ = (env && env[0]) ? env : "/tmp/prefetch_list.csv";

    ifstream in(path_);
    if (!in) {
        cerr << "[list_replayer] WARNING: could not open " << path_
             << "; no prefetches will be emitted." << endl;
        return;
    }

    string raw;
    uint64_t line_no = 0;
    while (getline(in, raw)) {
        ++line_no;
        const string line = trim_copy(raw);
        if (line.empty() || line[0] == '#') continue;

        // Strict two-column format. This intentionally rejects the rich
        // notebook CSV (order,pc,line,...), which otherwise parses pc as an
        // address and silently produces invalid replay.
        const size_t comma = line.find(',');
        if (comma == string::npos || line.find(',', comma + 1) != string::npos) {
            // Header is expected and harmless; all other malformed lines are counted.
            if (line_no != 1) ++rejected_lines_;
            continue;
        }

        const string idx_s = trim_copy(line.substr(0, comma));
        const string addr_s = trim_copy(line.substr(comma + 1));
        if (idx_s.empty() || addr_s.empty() || !std::isdigit(static_cast<unsigned char>(idx_s[0]))) {
            if (line_no != 1) ++rejected_lines_;
            continue;
        }

        try {
            const uint64_t idx = std::stoull(idx_s, nullptr, 10);
            const uint64_t addr = std::stoull(addr_s, nullptr, 0);
            if (addr == 0 || (addr & (BLOCK_SIZE - 1)) != 0) {
                ++rejected_lines_;
                continue;
            }
            table_[idx].push_back(addr);
            ++loaded_;
        } catch (const std::exception&) {
            ++rejected_lines_;
        }
    }

    cerr << "[list_replayer] loaded " << loaded_ << " prefetch entries across "
         << table_.size() << " ROI L2 LOAD indices from " << path_;
    if (rejected_lines_) cerr << " (rejected " << rejected_lines_ << " malformed lines)";
    cerr << endl;
}

void ListReplayer::invoke_prefetcher(uint64_t /*pc*/, uint64_t /*address*/,
                                     uint8_t /*cache_hit*/, uint8_t type,
                                     vector<uint64_t>& pref_addr)
{
    // CACHE::l2c_prefetcher_operate is normally invoked for LOAD only. Keep
    // this guard so an accidental alternate hook cannot change the index domain.
    if (type != LOAD) return;

    // Oracle demand_idx starts at the first post-warmup L2 LOAD. Do not count
    // or emit during warmup, otherwise all list indices shift.
    if (!warmup_complete[cache_->cpu]) return;

    const auto it = table_.find(counter_);
    if (it != table_.end()) {
        ++matched_;
        for (const uint64_t target : it->second) {
            pref_addr.push_back(target);
            ++emitted_;
        }
    }
    ++counter_;
}

void ListReplayer::dump_stats()
{
    cerr << "[list_replayer] emitted " << emitted_
         << " candidates over " << counter_ << " ROI L2 LOAD accesses ("
         << matched_ << " matched access indices)" << endl;
}

void ListReplayer::print_config()
{
    cout << "list_replayer_input " << path_ << endl
         << "list_replayer_index_domain ROI_L2_LOAD_after_warmup" << endl;
}
