// Disk-paged MoE experts (spec.md "Disk-paged MoE experts"). Model-free:
// the memory plan, the LRU mirror of llama.cpp's GPU expert cache, its slot
// arithmetic, page residency on a real mmap'd file and the ExpertPager fed
// routing by hand.
#include "easyai/paging.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace easyai::paging;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

static constexpr std::uint64_t GiB = 1024ull * 1024ull * 1024ull;

static GgufFootprint moe(std::uint64_t total, std::uint64_t experts) {
    GgufFootprint f;
    f.ok = true;
    f.arch = "test";
    f.tensor_bytes = total;
    f.expert_bytes = experts;
    f.n_layer = 60;
    f.n_expert = 256;
    f.n_expert_used = 8;
    return f;
}

int main() {
    // ---- names --------------------------------------------------------------
    CHECK(is_expert_tensor("blk.3.ffn_up_exps.weight"));
    CHECK(is_expert_tensor("blk.3.ffn_down_exps.weight"));
    CHECK(is_expert_tensor("blk.3.ffn_gate_exps.weight"));
    CHECK(is_expert_tensor("blk.3.ffn_gate_up_exps.weight"));
    CHECK(is_expert_tensor("blk.3.ffn_up_chexps.weight"));
    CHECK(!is_expert_tensor("blk.3.ffn_up_shexp.weight"));
    CHECK(!is_expert_tensor("blk.3.ffn_up.weight"));
    CHECK(!is_expert_tensor("blk.3.ffn_gate_inp.weight"));
    CHECK(layer_of_tensor("blk.12.ffn_up_exps.weight") == 12);
    CHECK(layer_of_tensor("output.weight") == -1);
    CHECK(layer_of_tensor("blk.x.ffn") == -1);
    CHECK(layer_of_node("ffn_moe_topk-7", "ffn_moe_topk") == 7);
    CHECK(layer_of_node("ffn_moe_topk-", "ffn_moe_topk") == -1);
    CHECK(layer_of_node("ffn_moe_topk-7 (view)", "ffn_moe_topk") == -1);
    CHECK(layer_of_node("ffn_moe_topk_x-7", "ffn_moe_topk") == -1);
    CHECK(layer_of_node(nullptr, "ffn_moe_topk") == -1);

    // ---- mode names -----------------------------------------------------------
    MemoryMode m = MemoryMode::Gpu;
    CHECK(parse_memory_mode("auto", m) && m == MemoryMode::Auto);
    CHECK(parse_memory_mode("PAGED", m) && m == MemoryMode::Paged);
    CHECK(parse_memory_mode("disk", m) && m == MemoryMode::Paged);
    CHECK(parse_memory_mode("gpu", m) && m == MemoryMode::Gpu);
    CHECK(!parse_memory_mode("swap", m));
    CHECK(std::string(memory_mode_name(MemoryMode::Paged)) == "paged");

    // ---- plan -----------------------------------------------------------------
    {   // ai-pro today: 80 GiB of weights on 91 GiB of unified RAM stays on the GPU path
        PlanInput in;
        in.model = moe(80 * GiB, 70 * GiB);
        in.ram_total = 91 * GiB;
        in.gpu_total = 85 * GiB;
        in.unified_memory = true;
        const auto p = plan_memory(in);
        CHECK(p.mode == MemoryMode::Gpu);
        CHECK(p.weight_budget == 91 * GiB - memory_reserve(91 * GiB));
    }
    {   // 150 GiB MoE on the same box → paged
        PlanInput in;
        in.model = moe(150 * GiB, 140 * GiB);
        in.ram_total = 91 * GiB;
        in.gpu_total = 85 * GiB;
        in.unified_memory = true;
        const auto p = plan_memory(in);
        CHECK(p.mode == MemoryMode::Paged);
        CHECK(p.reason.find("experts paged") != std::string::npos);
    }
    {   // the request wins over the size
        PlanInput in;
        in.model = moe(150 * GiB, 140 * GiB);
        in.ram_total = 91 * GiB;
        in.requested = MemoryMode::Gpu;
        CHECK(plan_memory(in).mode == MemoryMode::Gpu);
        in.model = moe(10 * GiB, 8 * GiB);
        in.requested = MemoryMode::Paged;
        CHECK(plan_memory(in).mode == MemoryMode::Paged);
    }
    {   // discrete 24 GiB card: 21 GiB fits, 30 GiB pages
        PlanInput in;
        in.ram_total = 64 * GiB;
        in.gpu_total = 24 * GiB;
        in.model = moe(21 * GiB, 18 * GiB);
        CHECK(plan_memory(in).mode == MemoryMode::Gpu);
        in.model = moe(30 * GiB, 26 * GiB);
        CHECK(plan_memory(in).mode == MemoryMode::Paged);
    }
    {   // unreadable footprint keeps the historical path
        PlanInput in;
        in.ram_total = 91 * GiB;
        in.model.ok = false;
        in.model.error = "nope";
        CHECK(plan_memory(in).mode == MemoryMode::Gpu);
    }
    {   // dense model too big: reason says layers are split
        PlanInput in;
        GgufFootprint f;
        f.ok = true; f.tensor_bytes = 120 * GiB; f.n_layer = 80;
        in.model = f;
        in.ram_total = 91 * GiB;
        const auto p = plan_memory(in);
        CHECK(p.mode == MemoryMode::Paged);
        CHECK(p.reason.find("dense") != std::string::npos);
    }
    CHECK(memory_reserve(16 * GiB) == 4 * GiB);
    CHECK(memory_reserve(128 * GiB) == 8 * GiB);

    // ---- cache sizing -----------------------------------------------------------
    CHECK(auto_expert_cache_bytes(60 * GiB, 10 * GiB, 200 * GiB, 80 * GiB, true) == 0);         // unified memory: off
    CHECK(auto_expert_cache_bytes(24 * GiB, 12 * GiB, 200 * GiB, 64 * GiB, false) == 11 * GiB); // discrete: 1 GiB margin
    CHECK(auto_expert_cache_bytes(24 * GiB, 12 * GiB, 5 * GiB, 64 * GiB, false) == 5 * GiB);    // capped at the experts
    CHECK(auto_expert_cache_bytes(13 * GiB, 12 * GiB, 200 * GiB, 64 * GiB, false) == 0);        // < 1 GiB left
    CHECK(dense_gpu_layers(81 * GiB, 80, 40 * GiB) == 39);
    CHECK(dense_gpu_layers(81 * GiB, 80, 500 * GiB) == 80);
    CHECK(dense_gpu_layers(81 * GiB, 80, 1 * GiB) == 0);

    // ---- slot arithmetic (llama_moe_cache: pad(nb2*(n+1)) per tensor) -------------
    {
        std::vector<ExpertTensorLayout> t(3);
        t[0].expert_bytes = 1000; t[1].expert_bytes = 1000; t[2].expert_bytes = 2000;
        CHECK(moe_cache_slots(t, 10000, 256, 100) == 1);   // n=2 needs 12288
        CHECK(moe_cache_slots(t, 12288, 256, 100) == 2);
        CHECK(moe_cache_slots(t, 1 << 30, 256, 7) == 7);   // capped at layers*n_expert
        CHECK(moe_cache_slots(t, 100, 256, 100) == 0);
    }

    // ---- LRU mirror: same touches and evictions as llama_moe_cache ---------------
    {
        ExpertLru lru;
        lru.init(2, 8, 4, 2);
        CHECK(lru.enabled());
        CHECK(lru.serves(1));
        CHECK(lru.serves(2));     // min(2*2, 8) = 4 <= 4
        CHECK(!lru.serves(3));    // 6 distinct could exceed 4 slots
        CHECK(!lru.serves(33));   // large ubatches never use the cache
        std::vector<std::int32_t> miss;
        std::size_t hit = 0;
        const std::int32_t a[] = {2, 1};
        CHECK(lru.plan(0, a, 2, miss, hit) && hit == 0 && miss.size() == 2 && miss[0] == 1 && miss[1] == 2);
        const std::int32_t b[] = {3, 2, 2};
        CHECK(lru.plan(0, b, 3, miss, hit) && hit == 1 && miss.size() == 1 && miss[0] == 3);
        const std::int32_t c[] = {5, 1};
        CHECK(lru.plan(1, c, 2, miss, hit) && hit == 0 && miss.size() == 2);
        CHECK(!lru.cached(0, 1));  // least recently used: evicted for layer 1's second miss
        CHECK(lru.cached(0, 2) && lru.cached(0, 3) && lru.cached(1, 1) && lru.cached(1, 5));
        const std::int32_t d[] = {0, 1, 2, 3, 4};
        CHECK(!lru.plan(0, d, 5, miss, hit));   // more distinct than slots
        ExpertLru small;
        small.init(2, 8, 1, 2);
        CHECK(!small.enabled());                // fewer slots than experts per token
    }

    // ---- residency + read-ahead on a real mmap -----------------------------------
    const std::size_t eb = 64 * 1024;          // one expert slice
    const int n_expert = 8;
    const std::size_t tensor = eb * n_expert;  // one [*, *, 8] expert tensor
    const std::size_t file = tensor * 3;       // gate, up, down
    char path[] = "easyai-test-paging-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    std::vector<unsigned char> blob(file, 0x5a);
    CHECK(write(fd, blob.data(), blob.size()) == (ssize_t) blob.size());
    fsync(fd);
    void * map = mmap(nullptr, file, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    const auto * base = (const unsigned char *) map;
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    std::printf("info dropped from page cache: %s (tmpfs keeps pages)\n",
                range_resident(base, file) ? "no" : "yes");
#endif
    {   // the mapping resolves back to the file, and RWF_NOWAIT sees the eviction
        const FileSpan fsp = file_of_address(base + 100);
        CHECK(fsp.ok);
        CHECK(fsp.path.find("easyai-test-paging-") != std::string::npos);
        CHECK(fsp.offset == 100);
        std::vector<char> heap(1 << 20);
        CHECK(!file_of_address(heap.data()).ok);   // anonymous memory: not a file
        const int r = file_range_resident(fd, 0, file);
        std::printf("info file probe after eviction: %d (0 = not cached, -1 = no RWF_NOWAIT)\n", r);
    }
    prefetch_range(base, file);   // must not crash on any alignment
    prefetch_range(base + 1, 100);
    volatile unsigned sum = 0;
    for (std::size_t i = 0; i < file; i += 4096) sum += base[i];
    CHECK(range_resident(base, file));
    CHECK(range_resident(base + 123, 4000));
    CHECK(file_range_resident(fd, 0, file) != 0);    // touched: cached (or -1 without RWF_NOWAIT)
    CHECK(file_range_resident(fd, 123, 4000) != 0);
    CHECK(range_resident(nullptr, 10));

    // ---- ExpertPager fed by hand -------------------------------------------------
    {
        ExpertPager::Config cfg;
        cfg.prefetch = true;
        cfg.gpu_alignment = 256;
        cfg.n_expert_used = 2;
        // room for exactly 4 experts of the 3 tensors (+1 padding slot each)
        cfg.gpu_cache_bytes = 3 * eb * 5;
        ExpertPager pager(cfg);
        std::vector<ExpertTensorLayout> lay(3);
        for (auto & l : lay) { l.type = 0; l.ne0 = 64; l.ne1 = 64; l.n_expert = n_expert; l.expert_bytes = eb; }
        pager.add_layer(4, {base, base + tensor, base + 2 * tensor}, lay);
        pager.finalize();
        auto st = pager.stats();
        CHECK(st.layers == 1);
        CHECK(st.gpu_cache_slots == 4);

        const std::int32_t t1[] = {1, 2};
        pager.observe(4, t1, 2, 1);          // two GPU misses, both read from RAM
        st = pager.stats();
        CHECK(st.ubatches == 1 && st.routed == 2 && st.gpu_hits == 0 && st.ram_hits == 2 && st.disk_reads == 0);
        pager.observe(4, t1, 2, 1);          // now both in the GPU cache
        st = pager.stats();
        CHECK(st.gpu_hits == 2 && st.ram_hits == 2);
        pager.observe(9, t1, 2, 1);          // untracked layer: ignored
        CHECK(pager.stats().routed == 4);

        // a 40-token ubatch routed to every expert: the 2 cached ones are
        // copied from the GPU cache, the other 6 come from host memory
        std::vector<std::int32_t> big;
        for (int i = 0; i < 40; ++i) { big.push_back(i % n_expert); big.push_back((i + 3) % n_expert); }
        pager.observe(4, big.data(), 2, 40);
        st = pager.stats();
        CHECK(st.routed == 12);
        CHECK(st.gpu_hits == 4);
        CHECK(st.ram_hits + st.disk_reads == 8);
        CHECK(st.prefetch_bytes == 0);       // everything was resident
    }
    {   // no GPU cache: every routed expert is read from host memory
        ExpertPager::Config cfg;
        cfg.n_expert_used = 2;
        ExpertPager pager(cfg);
        std::vector<ExpertTensorLayout> lay(3);
        for (auto & l : lay) { l.n_expert = n_expert; l.expert_bytes = eb; }
        pager.add_layer(0, {base, base + tensor, base + 2 * tensor}, lay);
        pager.finalize();
        CHECK(pager.stats().gpu_cache_slots == 0);
        const std::int32_t t1[] = {6, 7};
        pager.observe(0, t1, 2, 1);
        pager.observe(0, t1, 2, 1);
        const auto st = pager.stats();
        CHECK(st.gpu_hits == 0 && st.ram_hits == 4);
    }

    munmap(map, file);
    close(fd);
    unlink(path);
    (void) sum;

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
