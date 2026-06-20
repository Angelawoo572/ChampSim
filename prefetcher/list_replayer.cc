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
    load_reference();
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
        if (line_no == 1 && line == "idx,prefetch_addr") continue;

        if (!split_exact(line, 2, fields) || fields[0].empty() || fields[1].empty() ||
            !std::isdigit(static_cast<unsigned char>(fields[0][0]))) {
            ++rejected_lines_;
            continue;
        }

        try {
            const uint64_t idx = std::stoull(fields[0], nullptr, 10);
            const uint64_t addr = std::stoull(fields[1], nullptr, 0);
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

void ListReplayer::load_reference()
{
    const char* env = std::getenv("PFETCH_REF_PATH");
    reference_path_ = (env && env[0]) ? env : "";
    if (reference_path_.empty()) {
        cerr << "[list_replayer] WARNING: PFETCH_REF_PATH is unset; signature validation is DISABLED" << endl;
        return;
    }

    ifstream in(reference_path_);
    if (!in) {
        cerr << "[list_replayer] WARNING: could not open reference " << reference_path_
             << "; signature validation is DISABLED" << endl;
        return;
    }

    string raw;
    uint64_t line_no = 0;
    vector<string> fields;
    while (getline(in, raw)) {
        ++line_no;
        const string line = trim_copy(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line_no == 1 && line == "idx,pc,line") continue;

        if (!split_exact(line, 3, fields) || fields[0].empty() || fields[1].empty() || fields[2].empty() ||
            !std::isdigit(static_cast<unsigned char>(fields[0][0]))) {
            ++reference_rejected_lines_;
            continue;
        }
        try {
            const uint64_t idx = std::stoull(fields[0], nullptr, 10);
            const uint64_t pc = std::stoull(fields[1], nullptr, 0);
            const uint64_t line_addr = std::stoull(fields[2], nullptr, 0);
            if (idx != reference_.size()) {
                cerr << "[list_replayer] WARNING: non-contiguous reference idx at file line " << line_no
                     << "; signature validation is DISABLED" << endl;
                reference_.clear();
                return;
            }
            ReferenceSignature sig;
            sig.pc = pc;
            sig.line = line_addr;
            reference_.push_back(sig);
        } catch (const std::exception&) {
            ++reference_rejected_lines_;
        }
    }

    if (reference_.empty() || reference_rejected_lines_) {
        cerr << "[list_replayer] WARNING: invalid reference " << reference_path_
             << " (rows=" << reference_.size() << ", rejected=" << reference_rejected_lines_
             << "); signature validation is DISABLED" << endl;
        reference_.clear();
        return;
    }

    reference_enabled_ = true;
    cerr << "[list_replayer] loaded " << reference_.size()
         << " dense ROI L2 LOAD signatures from " << reference_path_ << endl;
}

void ListReplayer::invoke_prefetcher(uint64_t pc, uint64_t address,
                                     uint8_t /*cache_hit*/, uint8_t type,
                                     vector<uint64_t>& pref_addr)
{
    // CACHE::l2c_prefetcher_operate is normally invoked for LOAD only. Keep
    // this guard so an accidental alternate hook cannot change the index domain.
    if (type != LOAD) return;

    // Oracle demand_idx starts at the first post-warmup L2 LOAD. Do not count
    // or emit during warmup, otherwise all list indices shift.
    if (!warmup_complete[cache_->cpu]) return;

    bool signature_ok = true;
    if (reference_enabled_) {
        if (counter_ >= reference_.size()) {
            ++reference_tail_accesses_;
            signature_ok = false;
        } else {
            const ReferenceSignature& want = reference_[counter_];
            const uint64_t got_line = address >> LOG2_BLOCK_SIZE;
            if (want.pc != pc || want.line != got_line) {
                ++signature_mismatches_;
                signature_ok = false;
                if (signature_mismatches_ <= 5) {
                    cerr << "[list_replayer] signature mismatch at ROI L2 index " << counter_
                         << ": expected(pc=" << want.pc << ",line=" << want.line
                         << ") got(pc=" << pc << ",line=" << got_line << ")" << endl;
                }
            }
        }
    }

    // On a signature mismatch, suppress candidate emission rather than issuing
    // an otherwise-correct address at the wrong dynamic demand event.
    if (signature_ok) {
        const auto it = table_.find(counter_);
        if (it != table_.end()) {
            ++matched_;
            for (const uint64_t target : it->second) {
                pref_addr.push_back(target);
                ++emitted_;
            }
        }
    }
    ++counter_;
}

void ListReplayer::dump_stats()
{
    // Pythia calls l2c_prefetcher_final_stats() explicitly at normal completion.
    if (stats_dumped_) return;
    stats_dumped_ = true;
    cerr << "[list_replayer] emitted " << emitted_
         << " candidates over " << counter_ << " ROI L2 LOAD accesses ("
         << matched_ << " matched access indices; "
         << signature_mismatches_ << " signature mismatches; "
         << reference_tail_accesses_ << " post-reference tail accesses; "
         << (reference_enabled_ ? "reference enabled" : "reference DISABLED")
         << ")" << endl;
}

void ListReplayer::print_config()
{
    cout << "list_replayer_input " << path_ << endl
         << "list_replayer_reference " << (reference_path_.empty() ? "<unset>" : reference_path_) << endl
         << "list_replayer_index_domain ROI_L2_LOAD_after_warmup" << endl;
}
