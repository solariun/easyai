// paging.cpp — disk-paged MoE experts: footprint scan, memory plan, the LRU
// mirror of llama.cpp's GPU expert cache and the RAM-tier read-ahead.
// See include/easyai/paging.hpp and spec.md "Disk-paged MoE experts".
#include "easyai/paging.hpp"
#include "easyai/log.hpp"

#include "ggml.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "llama.h"
// llama_graph_reserve lives in llama.cpp's staging header (the same include
// common/fit.cpp uses). The sibling checkout is pinned, see spec.md.
#include "../src/llama-ext.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <regex>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#define EASYAI_PAGING_POSIX 1
#endif

namespace easyai {
namespace paging {

namespace fs = std::filesystem;
static constexpr std::uint64_t GiB = 1024ull * 1024ull * 1024ull;
static constexpr std::uint64_t MiB = 1024ull * 1024ull;

// ===========================================================================
// names
// ===========================================================================
bool is_expert_tensor(const std::string & name) {
    static const std::regex re("\\.ffn_(up|down|gate|gate_up)_(ch|)exps");
    return std::regex_search(name, re);
}

int layer_of_tensor(const std::string & name) {
    if (name.compare(0, 4, "blk.") != 0) return -1;
    std::size_t i = 4;
    int v = 0;
    bool any = false;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
        v = v * 10 + (name[i] - '0');
        if (v > 1000000) return -1;
        any = true;
        ++i;
    }
    if (!any || i >= name.size() || name[i] != '.') return -1;
    return v;
}

int layer_of_node(const char * name, const char * prefix) {
    if (!name || !prefix) return -1;
    const std::size_t n = std::strlen(prefix);
    if (std::strncmp(name, prefix, n) != 0 || name[n] != '-') return -1;
    const char * p = name + n + 1;
    if (*p == '\0') return -1;
    int v = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 1000000) return -1;
    }
    return v;
}

// ===========================================================================
// GGUF footprint
// ===========================================================================
static std::uint64_t kv_u64(const gguf_context * c, const std::string & key, bool & found) {
    found = false;
    const std::int64_t id = gguf_find_key(c, key.c_str());
    if (id < 0) return 0;
    found = true;
    switch (gguf_get_kv_type(c, id)) {
        case GGUF_TYPE_UINT8:  return gguf_get_val_u8(c, id);
        case GGUF_TYPE_INT8:   return (std::uint64_t) std::max<int>(0, gguf_get_val_i8(c, id));
        case GGUF_TYPE_UINT16: return gguf_get_val_u16(c, id);
        case GGUF_TYPE_INT16:  return (std::uint64_t) std::max<int>(0, gguf_get_val_i16(c, id));
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(c, id);
        case GGUF_TYPE_INT32:  return (std::uint64_t) std::max<std::int32_t>(0, gguf_get_val_i32(c, id));
        case GGUF_TYPE_UINT64: return gguf_get_val_u64(c, id);
        case GGUF_TYPE_INT64:  return (std::uint64_t) std::max<std::int64_t>(0, gguf_get_val_i64(c, id));
        case GGUF_TYPE_ARRAY:  {
            // per-layer arrays (e.g. expert_count on hybrid archs): the max
            if (gguf_get_arr_type(c, id) == GGUF_TYPE_UINT32 || gguf_get_arr_type(c, id) == GGUF_TYPE_INT32) {
                const std::size_t n = gguf_get_arr_n(c, id);
                const auto * d = (const std::int32_t *) gguf_get_arr_data(c, id);
                std::uint64_t m = 0;
                for (std::size_t i = 0; i < n; ++i) m = std::max<std::uint64_t>(m, (std::uint64_t) std::max(0, d[i]));
                return m;
            }
            found = false;
            return 0;
        }
        default: found = false; return 0;
    }
}

static bool scan_one(const std::string & path, GgufFootprint & f, bool first) {
    gguf_init_params p;
    p.no_alloc = true;
    p.ctx      = nullptr;
    gguf_context * c = gguf_init_from_file(path.c_str(), p);
    if (!c) {
        f.error = "cannot read GGUF header: " + path;
        return false;
    }
    if (first) {
        const std::int64_t id = gguf_find_key(c, "general.architecture");
        if (id >= 0 && gguf_get_kv_type(c, id) == GGUF_TYPE_STRING) f.arch = gguf_get_val_str(c, id);
        bool found = false;
        const std::string a = f.arch.empty() ? std::string() : f.arch + ".";
        f.n_layer       = (std::uint32_t) kv_u64(c, a + "block_count", found);
        f.n_expert      = (std::uint32_t) kv_u64(c, a + "expert_count", found);
        f.n_expert_used = (std::uint32_t) kv_u64(c, a + "expert_used_count", found);
    }
    const std::int64_t n = gguf_get_n_tensors(c);
    for (std::int64_t i = 0; i < n; ++i) {
        const std::uint64_t sz = gguf_get_tensor_size(c, i);
        f.tensor_bytes += sz;
        if (is_expert_tensor(gguf_get_tensor_name(c, i))) f.expert_bytes += sz;
    }
    gguf_free(c);
    std::error_code ec;
    f.file_bytes += (std::uint64_t) fs::file_size(path, ec);
    f.n_files++;
    return true;
}

GgufFootprint scan_gguf(const std::string & path_in) {
    GgufFootprint f;
    std::error_code ec;
    std::string path = path_in;
    if (fs::is_symlink(path, ec)) {
        fs::path r = fs::canonical(path, ec);
        if (!ec) path = r.string();
    }
    if (!scan_one(path, f, true)) return f;

    // split model: read the count from the first file, walk its siblings
    gguf_init_params p;
    p.no_alloc = true;
    p.ctx      = nullptr;
    int n_split = 1;
    if (gguf_context * c = gguf_init_from_file(path.c_str(), p)) {
        bool found = false;
        n_split = (int) std::max<std::uint64_t>(1, kv_u64(c, "split.count", found));
        gguf_free(c);
    }
    if (n_split > 1) {
        char prefix[4096];
        if (llama_split_prefix(prefix, sizeof(prefix), path.c_str(), 0, n_split) <= 0) {
            f.error = "split GGUF is not the first part (-00001-of-...): " + path;
            return f;
        }
        for (int i = 1; i < n_split; ++i) {
            char sp[4096];
            llama_split_path(sp, sizeof(sp), prefix, i, n_split);
            if (!scan_one(sp, f, false)) return f;
        }
    }
    f.ok = true;
    return f;
}

// ===========================================================================
// plan
// ===========================================================================
bool parse_memory_mode(const std::string & s_in, MemoryMode & out) {
    std::string s;
    for (char ch : s_in) s += (char) std::tolower((unsigned char) ch);
    if (s.empty() || s == "auto")                       { out = MemoryMode::Auto;  return true; }
    if (s == "gpu" || s == "full" || s == "vram")       { out = MemoryMode::Gpu;   return true; }
    if (s == "paged" || s == "disk" || s == "mmap")     { out = MemoryMode::Paged; return true; }
    return false;
}

const char * memory_mode_name(MemoryMode m) {
    switch (m) {
        case MemoryMode::Auto:  return "auto";
        case MemoryMode::Gpu:   return "gpu";
        case MemoryMode::Paged: return "paged";
    }
    return "auto";
}

std::uint64_t memory_reserve(std::uint64_t total) {
    return std::max<std::uint64_t>(4 * GiB, total / 16);
}

static std::string gib(std::uint64_t b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GiB", (double) b / (double) GiB);
    return buf;
}

MemoryPlan plan_memory(const PlanInput & in) {
    MemoryPlan p;
    const bool on_ram = in.unified_memory || in.gpu_total == 0;
    const std::uint64_t total = on_ram ? in.ram_total : in.gpu_total;
    // a discrete card keeps a smaller reserve: the KV cache already has to
    // fit beside the weights, so 1/32 (>= 512 MiB) only absorbs overhead
    const std::uint64_t reserve = on_ram ? memory_reserve(total)
                                         : std::max<std::uint64_t>(512 * MiB, total / 32);
    p.weight_budget = total > reserve ? total - reserve : 0;

    if (in.requested == MemoryMode::Gpu) {
        p.mode = MemoryMode::Gpu;
        p.reason = "memory_mode = gpu";
        return p;
    }
    if (in.requested == MemoryMode::Paged) {
        p.mode = MemoryMode::Paged;
        p.reason = "memory_mode = paged";
        return p;
    }
    if (!in.model.ok || total == 0) {
        p.mode = MemoryMode::Gpu;
        p.reason = !in.model.ok ? "auto: footprint unknown (" + in.model.error + ") — full GPU"
                                : "auto: memory size unknown — full GPU";
        return p;
    }
    const char * where = on_ram ? (in.gpu_total == 0 ? "RAM" : "unified RAM") : "VRAM";
    if (in.model.tensor_bytes <= p.weight_budget) {
        p.mode = MemoryMode::Gpu;
        p.reason = "auto: weights " + gib(in.model.tensor_bytes) + " fit in " +
                   gib(p.weight_budget) + " of " + where;
    } else {
        p.mode = MemoryMode::Paged;
        p.reason = "auto: weights " + gib(in.model.tensor_bytes) + " exceed " +
                   gib(p.weight_budget) + " of " + where +
                   (in.model.moe() ? " — routed experts paged from disk" : " — dense layers split GPU/disk");
    }
    return p;
}

std::uint64_t ram_total_bytes() {
#ifdef EASYAI_PAGING_POSIX
    const long pages = sysconf(_SC_PHYS_PAGES), ps = sysconf(_SC_PAGESIZE);
    if (pages > 0 && ps > 0) return (std::uint64_t) pages * (std::uint64_t) ps;
#endif
    return 0;
}

std::uint64_t ram_available_bytes() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    std::uint64_t kb = 0;
    std::string unit;
    while (f >> key >> kb >> unit) {
        if (key == "MemAvailable:") return kb * 1024ull;
    }
    return ram_total_bytes();
}

std::uint64_t auto_expert_cache_bytes(std::uint64_t available, std::uint64_t gpu_need,
                                      std::uint64_t expert_bytes, std::uint64_t ram_total,
                                      bool unified) {
    // Unified memory (iGPU / APU): off. The cache would hold copies of
    // experts in the same RAM the page cache needs, every miss is a
    // single-threaded copy plus a per-layer sync, and the CPU reads the
    // page cache at the same bandwidth the iGPU would. Measured on ai-pro
    // (890M, 2026-11-20, spec.md "Disk-paged MoE experts"): 14 tok/s with
    // no cache, 8.9 with 2 GiB, 9.5 with 8 GiB.
    if (unified) return 0;
    (void) ram_total;
    const std::uint64_t floor = 1 * GiB;
    if (available <= gpu_need + floor) return 0;
    std::uint64_t c = available - gpu_need - floor;
    c = std::min(c, expert_bytes);
    return c < 1 * GiB ? 0 : c;
}

int dense_gpu_layers(std::uint64_t tensor_bytes, std::uint32_t n_layer, std::uint64_t gpu_budget) {
    if (tensor_bytes == 0 || n_layer == 0) return 0;
    // the output head + embeddings weigh about one layer
    const std::uint64_t per = tensor_bytes / ((std::uint64_t) n_layer + 1);
    if (per == 0) return (int) n_layer;
    const std::uint64_t fit = gpu_budget / per;
    if (fit <= 1) return 0;
    return (int) std::min<std::uint64_t>(fit - 1, n_layer);
}

// ===========================================================================
// ExpertLru — mirror of llama_moe_cache's moe_cache_lru
// ===========================================================================
void ExpertLru::init(int n_layer, int n_expert, int n_slots, int n_expert_used) {
    n_expert_      = n_expert;
    n_expert_used_ = n_expert_used;
    n_slots_       = (n_slots >= n_expert_used && n_slots > 0) ? n_slots : 0;
    slot_.assign((std::size_t) std::max(0, n_layer) * (std::size_t) std::max(0, n_expert), -1);
    key_.assign(n_slots_, -1);
    prev_.resize(n_slots_);
    next_.resize(n_slots_);
    for (int s = 0; s < n_slots_; ++s) {
        prev_[s] = s - 1;
        next_[s] = s + 1 < n_slots_ ? s + 1 : -1;
    }
    head_ = n_slots_ > 0 ? 0 : -1;
    tail_ = n_slots_ - 1;
    seen_.assign(std::max(0, n_expert), 0);
    seen_gen_ = 0;
}

bool ExpertLru::serves(std::int64_t n_tokens) const {
    if (n_slots_ == 0 || n_tokens <= 0 || n_tokens > 32) return false;
    return std::min<std::int64_t>(n_tokens * n_expert_used_, n_expert_) <= n_slots_;
}

void ExpertLru::touch(int s) {
    if (s == tail_) return;
    if (prev_[s] >= 0) next_[prev_[s]] = next_[s];
    else               head_ = next_[s];
    prev_[next_[s]] = prev_[s];
    prev_[s] = tail_;
    next_[s] = -1;
    next_[tail_] = s;
    tail_ = s;
}

bool ExpertLru::cached(int il, int e) const {
    const std::size_t k = (std::size_t) il * (std::size_t) n_expert_ + (std::size_t) e;
    return n_slots_ > 0 && e >= 0 && e < n_expert_ && k < slot_.size() && slot_[k] >= 0;
}

bool ExpertLru::plan(int il, const std::int32_t * ids, std::size_t n_ids,
                     std::vector<std::int32_t> & misses, std::size_t & n_hit) {
    misses.clear();
    n_hit = 0;
    if (n_slots_ == 0) return false;
    if (++seen_gen_ == 0) {
        std::fill(seen_.begin(), seen_.end(), 0);
        seen_gen_ = 1;
    }
    uniq_.clear();
    for (std::size_t i = 0; i < n_ids; ++i) {
        const std::int32_t e = ids[i];
        if (e < 0 || e >= n_expert_) continue;
        if (seen_[e] != seen_gen_) {
            seen_[e] = seen_gen_;
            uniq_.push_back(e);
        }
    }
    if (uniq_.size() > (std::size_t) n_slots_) return false;

    std::int32_t * slots = slot_.data() + (std::size_t) il * (std::size_t) n_expert_;
    for (std::int32_t e : uniq_) {
        if (slots[e] >= 0) {
            touch(slots[e]);
            n_hit++;
        }
    }
    std::sort(uniq_.begin(), uniq_.end());
    for (std::int32_t e : uniq_) {
        if (slots[e] >= 0) continue;
        const int s = head_;
        if (key_[s] >= 0) slot_[key_[s]] = -1;
        key_[s]  = il * n_expert_ + e;
        slots[e] = s;
        touch(s);
        misses.push_back(e);
    }
    return true;
}

int moe_cache_slots(const std::vector<ExpertTensorLayout> & tensors, std::size_t budget,
                    std::size_t alignment, int max_slots) {
    if (tensors.empty() || alignment == 0) return 0;
    auto alloc = [&](int n) {
        std::size_t r = 0;
        for (const auto & t : tensors) {
            const std::size_t sz = t.expert_bytes * (std::size_t) (n + 1);
            r += (sz + alignment - 1) / alignment * alignment;
        }
        return r;
    };
    int n = 0;
    while (n < max_slots && alloc(n + 1) <= budget) n++;
    return n;
}

// ===========================================================================
// residency + read-ahead
// ===========================================================================
#ifdef EASYAI_PAGING_POSIX
static std::size_t page_size() {
    static const std::size_t ps = (std::size_t) std::max<long>(4096, sysconf(_SC_PAGESIZE));
    return ps;
}
#endif

bool range_resident(const void * p, std::size_t n) {
#ifdef EASYAI_PAGING_POSIX
    if (!p || n == 0) return true;
    const std::size_t ps = page_size();
    const std::uintptr_t b = (std::uintptr_t) p & ~(std::uintptr_t) (ps - 1);
    const std::uintptr_t e = ((std::uintptr_t) p + n + ps - 1) & ~(std::uintptr_t) (ps - 1);
    const std::size_t pages = (e - b) / ps;
    const std::size_t samples = std::min<std::size_t>(pages, 8);
    for (std::size_t i = 0; i < samples; ++i) {
        const std::size_t pg = samples == 1 ? 0 : i * (pages - 1) / (samples - 1);
#if defined(__APPLE__)
        char v = 0;
#else
        unsigned char v = 0;
#endif
        if (mincore((void *) (b + pg * ps), ps, &v) != 0) return true;  // not mapped: nothing to read
        if ((v & 1) == 0) return false;
    }
    return true;
#else
    (void) p; (void) n;
    return true;
#endif
}

FileSpan file_of_address(const void * p) {
    FileSpan r;
    std::ifstream f("/proc/self/maps");
    std::string line;
    const std::uintptr_t a = (std::uintptr_t) p;
    while (std::getline(f, line)) {
        unsigned long long lo = 0, hi = 0, off = 0;
        char perms[8] = {0};
        int n = 0;
        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &lo, &hi, perms, &off, &n) < 4) continue;
        if (a < lo || a >= hi) continue;
        std::string path = n > 0 && n <= (int) line.size() ? line.substr(n) : std::string();
        while (!path.empty() && (path.back() == ' ' || path.back() == '\n')) path.pop_back();
        if (path.empty() || path[0] != '/') return r;   // anonymous, [heap], ...
        const std::string del = " (deleted)";
        if (path.size() > del.size() && path.compare(path.size() - del.size(), del.size(), del) == 0) return r;
        r.ok = true;
        r.path = path;
        r.offset = (std::uint64_t) off + (std::uint64_t) (a - lo);
        return r;
    }
    return r;
}

int file_range_resident(int fd, std::uint64_t off, std::size_t n) {
#if defined(EASYAI_PAGING_POSIX) && defined(RWF_NOWAIT)
    if (fd < 0 || n == 0) return 1;
    const std::uint64_t ps = page_size();
    const std::uint64_t b = off & ~(ps - 1);
    const std::uint64_t pages = (off + n - b + ps - 1) / ps;
    const std::uint64_t samples = std::min<std::uint64_t>(pages, 8);
    for (std::uint64_t i = 0; i < samples; ++i) {
        const std::uint64_t pg = samples == 1 ? 0 : i * (pages - 1) / (samples - 1);
        std::uint64_t at = b + pg * ps;
        if (at < off) at = off;
        char c;
        struct iovec iov = { &c, 1 };
        const ssize_t r = preadv2(fd, &iov, 1, (off_t) at, RWF_NOWAIT);
        if (r < 0) {
            if (errno == EAGAIN) return 0;
            if (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL) return -1;
            return 1;   // unreadable: nothing to prefetch
        }
    }
    return 1;
#else
    (void) fd; (void) off; (void) n;
    return -1;
#endif
}

void prefetch_range(const void * p, std::size_t n) {
#ifdef EASYAI_PAGING_POSIX
    if (!p || n == 0) return;
    const std::size_t ps = page_size();
    const std::uintptr_t b = (std::uintptr_t) p & ~(std::uintptr_t) (ps - 1);
    const std::uintptr_t e = ((std::uintptr_t) p + n + ps - 1) & ~(std::uintptr_t) (ps - 1);
    (void) madvise((void *) b, e - b, MADV_WILLNEED);
#else
    (void) p; (void) n;
#endif
}

// ===========================================================================
// ExpertPager
// ===========================================================================
struct ExpertPager::Impl {
    Config cfg;

    struct Layer {
        int il = -1;
        std::vector<const std::uint8_t *>  data;    // host pointer per tensor (gate_up|gate, up, down)
        std::vector<ExpertTensorLayout>    layout;
        std::vector<int>                   fd;      // GGUF file of each tensor, -1 unknown
        std::vector<std::uint64_t>         off;     // file offset of each tensor's data
        int group = -1;
    };
    std::vector<int> fds;                // opened read-only for residency probes
    bool nowait_ok = true;               // RWF_NOWAIT works on these files

    ~Impl() {
#ifdef EASYAI_PAGING_POSIX
        for (int fd : fds) if (fd >= 0) close(fd);
#endif
    }

    // 1 resident, 0 not: the file probe when it works, mincore otherwise
    bool resident(const Layer & L, std::size_t k, const std::uint8_t * p, std::size_t n, std::uint64_t rel) {
        if (nowait_ok && L.fd[k] >= 0) {
            const int r = file_range_resident(L.fd[k], L.off[k] + rel, n);
            if (r >= 0) return r == 1;
            nowait_ok = false;
        }
        return range_resident(p, n);
    }
    struct Group {
        std::vector<ExpertTensorLayout> ref;
        std::vector<int> layers;
        std::uint64_t host_bytes = 0;
        ExpertLru lru;
    };

    std::vector<Layer> layers;
    std::vector<int>   index_of;      // il → index in layers, -1 untracked
    std::vector<Group> groups;
    int n_expert_used = 0;
    int first_il      = -1;
    bool ready        = false;

    // scratch, used only from the decode thread
    std::vector<std::int32_t> raw, ids, uniq, misses, host;
    std::vector<std::uint32_t> seen;
    std::uint32_t seen_gen = 0;

    std::atomic<std::uint64_t> ubatches{0}, routed{0}, gpu_hits{0}, ram_hits{0},
                               disk_reads{0}, prefetch_bytes{0};
    int gpu_slots = 0;

    bool tracked(int il) const {
        return ready && il >= 0 && il < (int) index_of.size() && index_of[il] >= 0;
    }
};

ExpertPager::ExpertPager(Config cfg) : p_(std::make_unique<Impl>()) {
    p_->cfg = cfg;
    p_->n_expert_used = cfg.n_expert_used;
}
ExpertPager::~ExpertPager() = default;

void ExpertPager::add_layer(int il, const std::vector<const void *> & data,
                            const std::vector<ExpertTensorLayout> & layout) {
    if (il < 0 || data.empty() || data.size() != layout.size()) return;
    if ((int) p_->index_of.size() <= il) p_->index_of.resize(il + 1, -1);
    if (p_->index_of[il] >= 0) return;
    Impl::Layer L;
    L.il = il;
    for (const void * d : data) L.data.push_back((const std::uint8_t *) d);
    L.layout = layout;
    p_->index_of[il] = (int) p_->layers.size();
    p_->layers.push_back(std::move(L));
}

void ExpertPager::finalize() {
    auto & I = *p_;
    std::sort(I.layers.begin(), I.layers.end(), [](const Impl::Layer & a, const Impl::Layer & b) { return a.il < b.il; });
    std::fill(I.index_of.begin(), I.index_of.end(), -1);
    I.groups.clear();
    std::uint64_t total_host = 0;
    for (std::size_t i = 0; i < I.layers.size(); ++i) {
        auto & L = I.layers[i];
        I.index_of[L.il] = (int) i;
        auto it = std::find_if(I.groups.begin(), I.groups.end(),
                               [&](const Impl::Group & g) { return g.ref == L.layout; });
        if (it == I.groups.end()) {
            I.groups.emplace_back();
            it = I.groups.end() - 1;
            it->ref = L.layout;
        }
        L.group = (int) (it - I.groups.begin());
        it->layers.push_back(L.il);
        for (const auto & t : L.layout) {
            it->host_bytes += (std::uint64_t) t.expert_bytes * (std::uint64_t) t.n_expert;
            total_host     += (std::uint64_t) t.expert_bytes * (std::uint64_t) t.n_expert;
        }
    }
    // where each tensor lives on disk, for truthful residency probes
    {
        std::vector<std::string> paths;
        for (int fd : I.fds) if (fd >= 0) {
#ifdef EASYAI_PAGING_POSIX
            close(fd);
#endif
        }
        I.fds.clear();
        for (auto & L : I.layers) {
            L.fd.assign(L.data.size(), -1);
            L.off.assign(L.data.size(), 0);
            for (std::size_t k = 0; k < L.data.size(); ++k) {
                const FileSpan fsp = file_of_address(L.data[k]);
                if (!fsp.ok) continue;
                std::size_t idx = std::find(paths.begin(), paths.end(), fsp.path) - paths.begin();
                if (idx == paths.size()) {
                    paths.push_back(fsp.path);
#ifdef EASYAI_PAGING_POSIX
                    I.fds.push_back(open(fsp.path.c_str(), O_RDONLY | O_CLOEXEC));
#else
                    I.fds.push_back(-1);
#endif
                }
                L.fd[k]  = I.fds[idx];
                L.off[k] = fsp.offset;
            }
        }
    }
    I.first_il = I.layers.empty() ? -1 : I.layers.front().il;
    I.gpu_slots = 0;
    const int n_layer = (int) I.index_of.size();
    for (auto & g : I.groups) {
        const int n_expert = (int) g.ref[0].n_expert;
        int n_slots = 0;
        if (I.cfg.gpu_cache_bytes > 0 && total_host > 0 && I.n_expert_used > 0) {
            const std::size_t budget = (std::size_t) ((double) I.cfg.gpu_cache_bytes *
                                                      (double) g.host_bytes / (double) total_host);
            n_slots = moe_cache_slots(g.ref, budget, I.cfg.gpu_alignment,
                                      (int) g.layers.size() * n_expert);
        }
        g.lru.init(n_layer, n_expert, n_slots, I.n_expert_used);
        I.gpu_slots += g.lru.n_slots();
    }
    int max_expert = 0;
    for (const auto & g : I.groups) max_expert = std::max<int>(max_expert, (int) g.ref[0].n_expert);
    I.seen.assign(max_expert, 0);
    I.seen_gen = 0;
    I.ready = !I.layers.empty();
}

int ExpertPager::attach(llama_context * ctx, int n_ubatch) {
    if (!ctx) return 0;
    // > 32 tokens: llama.cpp's MoE cache does not serve the ubatch, so the
    // graph's MUL_MAT_ID nodes read the host expert tensors themselves.
    const int n_tokens = std::min(64, std::max(1, n_ubatch));
    if (n_tokens <= 32 && p_->cfg.gpu_cache_bytes > 0) {
        easyai::log::write("[easyai] paging: n_ubatch=%d too small to locate the host experts — read-ahead off", n_ubatch);
        return 0;
    }
    ggml_cgraph * gf = nullptr;
    try {
        // every token an output: the last layer keeps all n_tokens rows
        // (with one output it would be trimmed to 1 row and use the cache)
        gf = llama_graph_reserve(ctx, (std::uint32_t) n_tokens, 1, (std::uint32_t) n_tokens);
    } catch (const std::exception & e) {
        easyai::log::error("[easyai] paging: graph reserve failed: %s", e.what());
        return 0;
    }
    if (!gf) return 0;

    // per layer: tensor role (0 gate_up, 1 gate, 2 up, 3 down) → tensor
    struct Seen { const ggml_tensor * t[4] = {nullptr, nullptr, nullptr, nullptr}; };
    std::vector<Seen> per_layer;
    int n_used = 0;
    const int n_nodes = ggml_graph_n_nodes(gf);
    const char * dbg = std::getenv("EASYAI_PAGING_DEBUG");
    const bool debug = dbg && *dbg && std::strcmp(dbg, "0") != 0;
    for (int i = 0; i < n_nodes; ++i) {
        const ggml_tensor * node = ggml_graph_node(gf, i);
        if (!node || node->op != GGML_OP_MUL_MAT_ID) continue;
        const ggml_tensor * w = node->src[0];
        // Once scheduled, a weight the GPU reads for a large ubatch is
        // replaced by its device copy "<backend>#<name>#<n>"; the host tensor
        // is still a leaf of the graph under its own name.
        if (w && (!w->buffer || !ggml_backend_buffer_is_host(w->buffer))) {
            const std::string cn = w->name;
            const std::size_t a = cn.find('#'), b = cn.rfind('#');
            if (a != std::string::npos && b != std::string::npos && b > a + 1) {
                const std::string orig = cn.substr(a + 1, b - a - 1);
                if (const ggml_tensor * h = ggml_graph_get_tensor(gf, orig.c_str())) w = h;
            }
        }
        if (debug && w) {
            std::fprintf(stderr, "[easyai] paging debug: %s <- %s data=%p buf=%s host=%d usage=%d view_src=%s\n",
                         node->name, w->name, w->data,
                         w->buffer ? ggml_backend_buffer_name(w->buffer) : "-",
                         w->buffer ? (int) ggml_backend_buffer_is_host(w->buffer) : -1,
                         w->buffer ? (int) ggml_backend_buffer_get_usage(w->buffer) : -1,
                         w->view_src ? w->view_src->name : "-");
        }
        if (!w || !w->data || !w->buffer || !ggml_backend_buffer_is_host(w->buffer)) continue;
        if (ggml_backend_buffer_get_usage(w->buffer) != GGML_BACKEND_BUFFER_USAGE_WEIGHTS) continue;
        const std::string name = w->name;
        const int il = layer_of_tensor(name);
        if (il < 0 || !is_expert_tensor(name)) continue;
        int role = -1;
        if      (name.find("ffn_gate_up_") != std::string::npos) role = 0;
        else if (name.find("ffn_gate_")    != std::string::npos) role = 1;
        else if (name.find("ffn_up_")      != std::string::npos) role = 2;
        else if (name.find("ffn_down_")    != std::string::npos) role = 3;
        if (role < 0) continue;
        if ((int) per_layer.size() <= il) per_layer.resize(il + 1);
        per_layer[il].t[role] = w;
        if (node->src[2]) n_used = std::max<int>(n_used, (int) node->src[2]->ne[0]);
    }
    if (n_used > 0) p_->n_expert_used = n_used;
    for (int il = 0; il < (int) per_layer.size(); ++il) {
        std::vector<const void *> data;
        std::vector<ExpertTensorLayout> layout;
        for (const ggml_tensor * t : per_layer[il].t) {
            if (!t) continue;
            ExpertTensorLayout l;
            l.type = (int) t->type;
            l.ne0 = t->ne[0];
            l.ne1 = t->ne[1];
            l.n_expert = t->ne[2];
            l.expert_bytes = t->nb[2];
            data.push_back(t->data);
            layout.push_back(l);
        }
        if (!data.empty()) add_layer(il, data, layout);
    }
    finalize();
    return (int) p_->layers.size();
}

bool ExpertPager::eval_callback(ggml_tensor * t, bool ask, void * user_data) {
    auto * self = static_cast<ExpertPager *>(user_data);
    if (!self || !t) return ask ? false : true;
    const int il = layer_of_node(t->name, "ffn_moe_topk");
    if (ask) return self->p_->tracked(il);
    if (!self->p_->tracked(il) || t->type != GGML_TYPE_I32 || !t->buffer) return true;

    const std::int64_t n_used = t->ne[0], n_tok = t->ne[1] * t->ne[2] * t->ne[3];
    if (n_used <= 0 || n_tok <= 0) return true;
    auto & I = *self->p_;
    const std::size_t span = ggml_nbytes(t);
    I.raw.resize(span / sizeof(std::int32_t) + 1);
    ggml_backend_tensor_get(t, I.raw.data(), 0, span);
    I.ids.resize((std::size_t) (n_used * n_tok));
    const std::size_t nb0 = t->nb[0] / sizeof(std::int32_t), nb1 = t->nb[1] / sizeof(std::int32_t);
    for (std::int64_t r = 0; r < n_tok; ++r)
        for (std::int64_t c = 0; c < n_used; ++c)
            I.ids[(std::size_t) (r * n_used + c)] = I.raw[(std::size_t) r * nb1 + (std::size_t) c * nb0];
    self->observe(il, I.ids.data(), n_used, n_tok);
    return true;
}

void ExpertPager::observe(int il, const std::int32_t * ids, std::int64_t n_used, std::int64_t n_tokens) {
    auto & I = *p_;
    if (!I.tracked(il) || !ids || n_used <= 0 || n_tokens <= 0) return;
    const auto & L = I.layers[I.index_of[il]];
    auto & G = I.groups[L.group];
    const int n_expert = (int) G.ref[0].n_expert;
    const std::size_t n_ids = (std::size_t) (n_used * n_tokens);

    if (il == I.first_il) I.ubatches.fetch_add(1, std::memory_order_relaxed);

    // distinct routed experts of this ubatch
    if (++I.seen_gen == 0) {
        std::fill(I.seen.begin(), I.seen.end(), 0);
        I.seen_gen = 1;
    }
    I.uniq.clear();
    for (std::size_t i = 0; i < n_ids; ++i) {
        const std::int32_t e = ids[i];
        if (e < 0 || e >= n_expert || e >= (int) I.seen.size()) continue;
        if (I.seen[e] != I.seen_gen) {
            I.seen[e] = I.seen_gen;
            I.uniq.push_back(e);
        }
    }
    I.routed.fetch_add(I.uniq.size(), std::memory_order_relaxed);

    // which of them are read from host memory: the GPU cache's misses for a
    // small ubatch; for a large one every routed expert the cache does not
    // already hold (large ubatches copy from the cache but never fill it)
    I.host.clear();
    if (G.lru.serves(n_tokens)) {
        std::size_t n_hit = 0;
        if (G.lru.plan(il, ids, n_ids, I.misses, n_hit)) {
            I.gpu_hits.fetch_add(n_hit, std::memory_order_relaxed);
            I.host = I.misses;
        } else {
            I.host = I.uniq;
        }
    } else {
        std::size_t n_hit = 0;
        for (std::int32_t e : I.uniq) {
            if (G.lru.cached(il, e)) n_hit++;
            else I.host.push_back(e);
        }
        I.gpu_hits.fetch_add(n_hit, std::memory_order_relaxed);
    }

    std::uint64_t queued = 0;
    for (std::int32_t e : I.host) {
        bool on_disk = false;
        for (std::size_t k = 0; k < L.data.size(); ++k) {
            const std::size_t eb = L.layout[k].expert_bytes;
            const std::uint8_t * p = L.data[k] + (std::size_t) e * eb;
            if (!I.resident(L, k, p, eb, (std::uint64_t) e * eb)) {
                on_disk = true;
                if (I.cfg.prefetch) {
                    prefetch_range(p, eb);
                    queued += eb;
                }
            }
        }
        if (on_disk) I.disk_reads.fetch_add(1, std::memory_order_relaxed);
        else         I.ram_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (queued) I.prefetch_bytes.fetch_add(queued, std::memory_order_relaxed);
}

PagerStats ExpertPager::stats() const {
    PagerStats s;
    s.ubatches        = p_->ubatches.load(std::memory_order_relaxed);
    s.routed          = p_->routed.load(std::memory_order_relaxed);
    s.gpu_hits        = p_->gpu_hits.load(std::memory_order_relaxed);
    s.ram_hits        = p_->ram_hits.load(std::memory_order_relaxed);
    s.disk_reads      = p_->disk_reads.load(std::memory_order_relaxed);
    s.prefetch_bytes  = p_->prefetch_bytes.load(std::memory_order_relaxed);
    s.layers          = (int) p_->layers.size();
    s.gpu_cache_slots = p_->gpu_slots;
    return s;
}

}  // namespace paging
}  // namespace easyai
