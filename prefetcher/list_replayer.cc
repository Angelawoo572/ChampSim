#include "list_replayer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "cache.h"
#include "champsim.h"

using namespace std;

namespace {
string trim_copy(string s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool split_exact(const string& line, size_t fields_needed, vector<string>& fields)
{
    fields.clear();
    string item;
    stringstream ss(line);
    while (getline(ss, item, ',')) fields.push_back(trim_copy(item));
    return fields.size() == fields_needed;
}
} // namespace

ListReplayer::ListReplayer(string type, CACHE* cache)
    : Prefetcher(type), cache_(cache)
{
    load_table();
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
    vector<string> fields;
    while (getline(in, raw)) {
        ++line_no;
        const string line = trim_copy(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line_no == 1 && line == "pc,line,occ,prefetch_addr") continue;

        if (!split_exact(line, 4, fields) || fields[0].empty() || fields[1].empty() ||
            fields[2].empty() || fields[3].empty() ||
            !std::isdigit(static_cast<unsigned char>(fields[0][0]))) {
            ++rejected_lines_;
            continue;
        }

        try {
            TriggerKey key;
            key.pc = std::stoull(fields[0], nullptr, 0);
            key.line = std::stoull(fields[1], nullptr, 0);
            key.occ = std::stoull(fields[2], nullptr, 10);
            const uint64_t addr = std::stoull(fields[3], nullptr, 0);
            if (addr == 0 || (addr & (BLOCK_SIZE - 1)) != 0) {
                ++rejected_lines_;
                continue;
            }
            table_[key].push_back(addr);
            ++loaded_;
        } catch (const std::exception&) {
            ++rejected_lines_;
        }
    }

    cerr << "[list_replayer] loaded " << loaded_ << " prefetch entries across "
         << table_.size() << " PC-line-occ triggers from " << path_;
    if (rejected_lines_) cerr << " (rejected " << rejected_lines_ << " malformed lines)";
    cerr << endl;
}

void ListReplayer::invoke_prefetcher(uint64_t pc, uint64_t address,
                                     uint8_t /*cache_hit*/, uint8_t type,
                                     vector<uint64_t>& pref_addr)
{
    // CACHE::l2c_prefetcher_operate normally invokes an L2 prefetcher for LOADs.
    // Keep this guard so the trigger domain remains demand L2 LOAD only.
    if (type != LOAD) return;

    // The oracle table begins at the first post-warmup L2 LOAD.
    if (!warmup_complete[cache_->cpu]) return;

    const uint64_t line = address >> LOG2_BLOCK_SIZE;
    PairKey pair;
    pair.pc = pc;
    pair.line = line;

    // Use the occurrence before incrementing: first occurrence is occ=0.
    const uint64_t occ = occurrences_[pair]++;
    TriggerKey key;
    key.pc = pc;
    key.line = line;
    key.occ = occ;

    const auto it = table_.find(key);
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
    if (stats_dumped_) return;
    stats_dumped_ = true;
    cerr << "[list_replayer] emitted " << emitted_
         << " candidates over " << counter_ << " runtime ROI L2 LOAD accesses ("
         << matched_ << " matched PC-line-occ triggers; "
         << table_.size() << " loaded trigger keys; key=pc_line_occ)" << endl;
}

void ListReplayer::print_config()
{
    cout << "list_replayer_input " << path_ << endl
         << "list_replayer_trigger_key pc_line_occ" << endl
         << "list_replayer_domain ROI_L2_LOAD_after_warmup" << endl;
}
