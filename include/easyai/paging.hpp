// easyai/paging.hpp — run models larger than memory: MoE experts paged from
// disk, a RAM page cache and a GPU cache of the hot experts.
//
// Three tiers, one per kind of memory:
//
//   disk (the GGUF, mmap'd)            every routed expert lives here
//     ↓  page faults + ExpertPager read-ahead (MADV_WILLNEED)
//   RAM page cache (kernel)            recently read experts, reclaimable
//     ↓  llama.cpp copy callback (moe_cache_size)
//   GPU hot-expert cache (llama.cpp)   LRU of the experts used in generation
//                                      (discrete GPUs; off by default on an
//                                      iGPU, where the CPU computes the
//                                      experts from the page cache faster)
//
// Engine::memory_mode("auto"|"gpu"|"paged") picks the tiers at load():
// "gpu" is the historical full-offload path (every weight in GPU memory,
// mlock/no_mmap honoured); "paged" keeps the routed experts of a MoE model
// in the mmap'd file and everything else (attention, shared experts,
// embeddings, KV cache) on the GPU; "auto" chooses "paged" only when the
// weights cannot fit in memory. See spec.md "Disk-paged MoE experts".
//
// Everything here except ExpertPager's ggml hook is plain C++ so the
// planning and cache logic are unit-tested without a model
// (tests/test_paging.cpp).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ggml_tensor;
struct llama_context;

namespace easyai {
namespace paging {

// ---------------------------------------------------------------------------
// GGUF footprint — what the weights need, read from the tensor table only
// ---------------------------------------------------------------------------
struct GgufFootprint {
    bool          ok            = false;
    std::string   error;
    std::string   arch;
    std::uint64_t file_bytes    = 0;   // all split files together
    std::uint64_t tensor_bytes  = 0;   // every tensor
    std::uint64_t expert_bytes  = 0;   // routed experts (blk.N.ffn_{gate,up,down,gate_up}_exps)
    std::uint32_t n_files       = 0;
    std::uint32_t n_layer       = 0;   // <arch>.block_count
    std::uint32_t n_expert      = 0;   // <arch>.expert_count (0 = dense)
    std::uint32_t n_expert_used = 0;   // <arch>.expert_used_count
    bool moe() const { return expert_bytes > 0 && n_expert > 0; }
};

// Reads the GGUF header of `path` and, for a split model
// (name-00001-of-0000N.gguf), of every sibling split.
GgufFootprint scan_gguf(const std::string & path);

// true when `tensor_name` is a routed-expert weight — the same tensors as
// llama.cpp's LLM_FFN_EXPS_REGEX ("\.ffn_(up|down|gate|gate_up)_(ch|)exps").
bool is_expert_tensor(const std::string & tensor_name);

// "blk.12.ffn_up_exps.weight" → 12; -1 when the name has no block index.
int layer_of_tensor(const std::string & tensor_name);

// "ffn_moe_topk-12" with prefix "ffn_moe_topk" → 12; -1 otherwise.
// Matches the names llama.cpp's graph callback gives MoE nodes ("%s-%d").
int layer_of_node(const char * node_name, const char * prefix);

// ---------------------------------------------------------------------------
// Memory plan — pure decision, unit-tested
// ---------------------------------------------------------------------------
enum class MemoryMode { Auto, Gpu, Paged };

// "auto" | "gpu" | "paged" (also "full"/"vram" → Gpu, "disk"/"mmap" → Paged).
// Returns false for anything else.
bool parse_memory_mode(const std::string & s, MemoryMode & out);
const char * memory_mode_name(MemoryMode m);

struct PlanInput {
    MemoryMode    requested       = MemoryMode::Auto;
    GgufFootprint model;
    std::uint64_t ram_total       = 0;   // MemTotal
    std::uint64_t gpu_total       = 0;   // 0 = no GPU device
    bool          unified_memory  = false; // iGPU / APU: GPU memory IS system RAM
};

struct MemoryPlan {
    MemoryMode    mode = MemoryMode::Gpu;   // Gpu or Paged, never Auto
    std::string   reason;                   // one line for the log / health
    std::uint64_t weight_budget = 0;        // memory the weights may use when fully loaded
};

// Reserve kept free for the OS, the voice models and the page cache when
// deciding whether weights "fit": max(4 GiB, total/16).
std::uint64_t memory_reserve(std::uint64_t total);

// Auto rule: weights fit when tensor_bytes <= budget, where budget is
// RAM − reserve on unified-memory or CPU-only machines and VRAM − reserve
// on a discrete GPU. A model that fits keeps the full-GPU path untouched.
MemoryPlan plan_memory(const PlanInput & in);

// Physical RAM (MemTotal) and what can still be had without swapping
// (MemAvailable on Linux; total on platforms without it). 0 = unknown.
std::uint64_t ram_total_bytes();
std::uint64_t ram_available_bytes();

// Size of the GPU hot-expert cache when expert_cache = auto.
//   available    GPU memory still free
//   gpu_need     what the non-expert weights + KV + compute buffers take
//   expert_bytes upper bound (caching more than every expert is useless)
//   unified      iGPU / APU: always 0 — the cache would copy experts into
//                the RAM the page cache needs, and the CPU computes them
//                from the page cache as fast (measured, see spec.md)
// Discrete GPU: available − gpu_need − 1 GiB margin, capped at
// expert_bytes; 0 when less than 1 GiB would be left for it.
std::uint64_t auto_expert_cache_bytes(std::uint64_t available, std::uint64_t gpu_need,
                                      std::uint64_t expert_bytes, std::uint64_t ram_total,
                                      bool unified);

// Dense model too big for the GPU: how many of its n_layer layers go to
// the GPU (the rest are read from the mmap'd file by the CPU backend).
int dense_gpu_layers(std::uint64_t tensor_bytes, std::uint32_t n_layer, std::uint64_t gpu_budget);

// ---------------------------------------------------------------------------
// LRU mirror of llama.cpp's MoE cache (src/llama-moe-cache.cpp)
// ---------------------------------------------------------------------------
// The GPU cache decides which experts it uploads from host memory; it does
// not say. ExpertPager replays the same LRU (same slot count, same order of
// touches and evictions) so it knows which routed experts will be read from
// the mmap and prefetches exactly those — never an expert the GPU already
// holds.
class ExpertLru {
public:
    // n_layer: layers indexed by the caller; n_expert per layer; n_slots in
    // the group. A group with fewer slots than n_expert_used is not cached
    // (llama.cpp disables it) — check enabled().
    void init(int n_layer, int n_expert, int n_slots, int n_expert_used);
    bool enabled() const { return n_slots_ > 0; }
    int  n_slots() const { return n_slots_; }

    // Same rule as llama_moe_cache::get_slot_map: the cache serves a ubatch
    // of n_tokens only when it is small (<= 32 tokens) and its distinct
    // experts can fit.
    bool serves(std::int64_t n_tokens) const;

    // Touch the experts in `ids` (any order, duplicates allowed) for layer
    // `il`; fills `misses` with the distinct experts the cache will upload
    // from host memory. Returns false (and plans nothing) when the distinct
    // experts outnumber the slots — llama.cpp aborts in that case, so it
    // never happens when serves() was true.
    bool plan(int il, const std::int32_t * ids, std::size_t n_ids,
              std::vector<std::int32_t> & misses, std::size_t & n_hit);

    bool cached(int il, int expert) const;

private:
    void touch(int s);
    int n_expert_ = 0, n_slots_ = 0, n_expert_used_ = 0;
    std::vector<std::int32_t> slot_;     // [n_layer * n_expert] → slot or -1
    std::vector<std::int32_t> key_;      // [n_slots] → il*n_expert + e or -1
    std::vector<std::int32_t> prev_, next_;
    int head_ = -1, tail_ = -1;
    std::vector<std::uint32_t> seen_;
    std::uint32_t seen_gen_ = 0;
    std::vector<std::int32_t> uniq_;
};

// One expert weight tensor of a layer, as llama.cpp's cache groups them.
struct ExpertTensorLayout {
    int           type   = -1;     // ggml_type
    std::int64_t  ne0    = 0, ne1 = 0, n_expert = 0;
    std::size_t   expert_bytes = 0; // nb[2]
    bool operator==(const ExpertTensorLayout & o) const {
        return type == o.type && ne0 == o.ne0 && ne1 == o.ne1 &&
               n_expert == o.n_expert && expert_bytes == o.expert_bytes;
    }
};

// llama_moe_cache's slot count for one group of layers sharing a layout:
// the largest n with sum over tensors of pad(expert_bytes*(n+1), alignment)
// <= budget, capped at layers*n_expert. The budget of each group is the
// cache size times the group's share of the host expert bytes.
int moe_cache_slots(const std::vector<ExpertTensorLayout> & tensors, std::size_t budget,
                    std::size_t alignment, int max_slots);

// ---------------------------------------------------------------------------
// Page residency + read-ahead on mmap'd memory
// ---------------------------------------------------------------------------
// true when every sampled page of [p, p+n) is in the page cache (first,
// last and up to 6 evenly spaced pages: a bounded mincore cost per expert).
// Only truthful for anonymous memory and files the process may write: use
// file_range_resident for the GGUF mapping.
bool range_resident(const void * p, std::size_t n);
// The file and offset a mapped address comes from, read from
// /proc/self/maps. ok=false for anonymous memory or without /proc.
struct FileSpan {
    bool          ok = false;
    std::string   path;
    std::uint64_t offset = 0;
};
FileSpan file_of_address(const void * p);
// Page-cache residency of [off, off+n) of an open file, sampled like
// range_resident: preadv2(RWF_NOWAIT) of one byte per sample. Unlike
// mincore() it is truthful on files the process cannot write — since Linux
// 5.0 mincore() reports such mappings as fully resident. Returns 1 cached,
// 0 not cached, -1 when the kernel or filesystem lacks RWF_NOWAIT.
int file_range_resident(int fd, std::uint64_t off, std::size_t n);
// madvise(MADV_WILLNEED) on the page-aligned cover of [p, p+n): the kernel
// queues the reads asynchronously, so a whole expert arrives as one large
// read instead of 128 KiB page-fault read-arounds.
void prefetch_range(const void * p, std::size_t n);

// ---------------------------------------------------------------------------
// ExpertPager — the RAM tier: sees routing, reads ahead, counts
// ---------------------------------------------------------------------------
struct PagerStats {
    std::uint64_t ubatches        = 0;  // ubatches whose routing was seen
    std::uint64_t routed          = 0;  // distinct (layer, expert) per ubatch
    std::uint64_t gpu_hits        = 0;  // served by the GPU hot-expert cache
    std::uint64_t ram_hits        = 0;  // read from RAM (page cache)
    std::uint64_t disk_reads      = 0;  // had to come from disk
    std::uint64_t prefetch_bytes  = 0;  // bytes queued for read-ahead
    int           layers          = 0;  // MoE layers tracked
    int           gpu_cache_slots = 0;  // experts the GPU cache holds (mirror)
};

class ExpertPager {
public:
    struct Config {
        bool        prefetch        = true;
        std::size_t gpu_cache_bytes = 0;    // the moe_cache_size handed to llama.cpp
        std::size_t gpu_alignment   = 256;  // buffer alignment of the GPU device
        int         n_expert_used   = 0;    // attach() reads it from the graph
    };

    explicit ExpertPager(Config cfg);
    ~ExpertPager();
    ExpertPager(const ExpertPager &) = delete;
    ExpertPager & operator=(const ExpertPager &) = delete;

    // Learns each MoE layer's host expert tensors from a reserved graph
    // (n_tokens > 32 so the graph reads the host weights, not the cache
    // banks) and sizes the LRU mirror. Returns the number of layers found.
    int attach(llama_context * ctx, int n_ubatch);

    // ggml_backend_sched_eval_callback: asks for "ffn_moe_topk-<il>" of
    // tracked layers, reads the selected experts, plans, prefetches.
    static bool eval_callback(ggml_tensor * t, bool ask, void * user_data);

    // Feed routing directly (what eval_callback does after reading ids).
    // ids: n_tokens rows of n_used int32. Public for the unit test.
    void observe(int il, const std::int32_t * ids, std::int64_t n_used, std::int64_t n_tokens);

    // Registers a layer by hand (attach() does this from the graph). Public
    // for the unit test, which pages a temp file instead of a model.
    void add_layer(int il, const std::vector<const void *> & data,
                   const std::vector<ExpertTensorLayout> & layout);
    // Builds the LRU mirror groups once every layer is registered.
    void finalize();

    PagerStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace paging
}  // namespace easyai
