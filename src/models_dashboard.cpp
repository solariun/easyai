// src/models_dashboard.cpp — native MODELS dashboard engine.
//
// See include/easyai/models_dashboard.hpp for the contract. This is a native
// C++ re-implementation of llmfit's hardware-aware model scoring (no external
// binary), plus GGUF introspection and a libcurl download manager.
//
// The scoring is a faithful-but-pragmatic port of llmfit-core (fit.rs /
// models.rs / hardware.rs / plan.rs): the memory model, fit levels, quant
// selection, tok/s estimation and the four score components match llmfit's
// formulas and constants; a few exotic branches (MoE bandwidth decomposition,
// the full GPU bandwidth table) are approximated — results may differ from
// llmfit at the margins, which is expected and acceptable.

#include "easyai/models_dashboard.hpp"
#include "easyai/config.hpp"
#include "easyai/log.hpp"

#include <nlohmann/json.hpp>

#include "llama.h"
#include "ggml-backend.h"
#include "gguf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <future>
#include <regex>
#include <sstream>

#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#endif

#if defined(EASYAI_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace easyai {

namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace {

// ===========================================================================
// small helpers
// ===========================================================================
std::string to_upper(std::string s) {
    for (char & c : s) c = (char) std::toupper((unsigned char) c);
    return s;
}
std::string to_lower(std::string s) {
    for (char & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}
bool contains_ci(const std::string & hay, const std::string & needle) {
    return to_lower(hay).find(to_lower(needle)) != std::string::npos;
}
bool ends_with_ci(const std::string & s, const std::string & suf) {
    if (s.size() < suf.size()) return false;
    return std::equal(suf.rbegin(), suf.rend(), s.rbegin(),
                      [](char a, char b) {
                          return std::tolower((unsigned char) a) ==
                                 std::tolower((unsigned char) b);
                      });
}
double round1(double v) { return std::round(v * 10.0) / 10.0; }
double round2(double v) { return std::round(v * 100.0) / 100.0; }
double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Parse "key=value&key2=value2" (already URL-decoded by httplib) into a map.
std::map<std::string, std::string> parse_query(const std::string & q) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        std::string pair = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) out[pair.substr(0, eq)] = pair.substr(eq + 1);
        else if (!pair.empty())      out[pair] = "";
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}
std::string qget(const std::map<std::string, std::string> & q, const std::string & k,
                 const std::string & def = "") {
    auto it = q.find(k);
    return it == q.end() ? def : it->second;
}
double qnum(const std::map<std::string, std::string> & q, const std::string & k, double def) {
    auto it = q.find(k);
    if (it == q.end() || it->second.empty()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}

// ===========================================================================
// quant tables (llmfit models.rs)
// ===========================================================================
// bytes-per-weight for MEMORY estimation (quant_bpp, models.rs:11)
double quant_bpp(const std::string & q) {
    std::string u = to_upper(q);
    auto has = [&](const char * s) { return u.find(s) != std::string::npos; };
    if (u == "F32" || has("F32")) return 4.0;
    if (has("F16") || has("BF16")) return 2.0;
    if (has("Q8")) return 1.05;
    if (has("Q6")) return 0.80;
    if (has("Q5")) return 0.68;
    if (has("Q4")) return 0.58;
    if (has("Q3")) return 0.48;
    if (has("Q2")) return 0.37;
    if (has("MLX-8")) return 1.0;
    if (has("MLX-4")) return 0.55;
    if (has("8BIT") || has("INT8")) return 1.0;
    if (has("4BIT") || has("INT4")) return 0.5;
    if (has("IQ4")) return 0.50;
    if (has("IQ3")) return 0.42;
    if (has("IQ2")) return 0.33;
    if (has("IQ1")) return 0.22;
    return 0.58;  // default
}
// bytes-per-weight for SPEED (quant_bytes_per_param, models.rs:63)
double quant_bytes_per_param(const std::string & q) {
    std::string u = to_upper(q);
    auto has = [&](const char * s) { return u.find(s) != std::string::npos; };
    if (has("F16") || has("BF16")) return 2.0;
    if (has("Q8") || has("8BIT") || has("INT8")) return 1.0;
    if (has("Q6")) return 0.75;
    if (has("Q5")) return 0.625;
    if (has("Q4") || has("4BIT") || has("INT4") || has("MLX-4")) return 0.5;
    if (has("Q3")) return 0.375;
    if (has("Q2")) return 0.25;
    if (has("MLX-8")) return 1.0;
    return 0.5;
}
// speed multiplier for the fixed-K fallback (quant_speed_multiplier, models.rs:38)
double quant_speed_mult(const std::string & q) {
    std::string u = to_upper(q);
    auto has = [&](const char * s) { return u.find(s) != std::string::npos; };
    if (has("F16") || has("BF16")) return 0.6;
    if (has("Q8") || has("8BIT")) return 0.8;
    if (has("Q6")) return 0.95;
    if (has("Q5")) return 1.0;
    if (has("Q4") || has("4BIT")) return 1.15;
    if (has("Q3")) return 1.25;
    if (has("Q2")) return 1.35;
    return 1.0;
}
// quality penalty (quant_quality_penalty, models.rs:87) — negative
double quant_quality_penalty(const std::string & q) {
    std::string u = to_upper(q);
    auto has = [&](const char * s) { return u.find(s) != std::string::npos; };
    if (has("F16") || has("BF16") || has("Q8") || has("8BIT")) return 0.0;
    if (has("Q6")) return -1.0;
    if (has("Q5")) return -2.0;
    if (has("Q4") || has("4BIT")) return -5.0;
    if (has("Q3")) return -8.0;
    if (has("Q2")) return -12.0;
    if (has("MLX-4")) return -4.0;
    return -5.0;
}
double kv_bytes_per_elem(const std::string & kv) {
    std::string u = to_upper(kv);
    if (u == "FP8" || u == "Q8_0") return 1.0;
    if (u == "Q4_0") return 0.5;
    if (u == "TQ" || u == "TURBOQUANT") return 0.34;
    return 2.0;  // fp16 default
}

// GGUF quant hierarchy, best→worst (models.rs:5)
const std::vector<std::string> kQuantHierarchy =
    { "Q8_0", "Q6_K", "Q5_K_M", "Q4_K_M", "Q3_K_M", "Q2_K" };

// ===========================================================================
// GPU memory bandwidth table (GB/s) — hardware.rs:1853. Substring match,
// most-specific first. A representative subset; unmatched names fall to the
// fixed-K speed path (so this never breaks, only loses precision).
// ===========================================================================
std::optional<double> gpu_bandwidth_gbps(const std::string & name_in) {
    std::string n = to_lower(name_in);
    struct E { const char * key; double bw; };
    static const E table[] = {
        // NVIDIA 50
        {"5090",1792},{"5080",960},{"5070 ti",896},{"5070",672},{"5060 ti",448},{"5060",256},
        // NVIDIA 40
        {"4090",1008},{"4080 super",736},{"4080",717},{"4070 ti super",672},{"4070 ti",504},
        {"4070 super",504},{"4070",504},{"4060 ti",288},{"4060",272},
        // NVIDIA 30
        {"3090 ti",1008},{"3090",936},{"3080 ti",912},{"3080",760},{"3070 ti",608},
        {"3070",448},{"3060 ti",448},{"3060",360},
        // NVIDIA 20 / 16
        {"2080 ti",616},{"2080",448},{"2070",448},{"2060",336},{"1660",288},{"1650",128},
        // datacenter
        {"h200",4800},{"h100",2039},{"a100",1555},{"l40s",864},{"l40",864},{"l4",300},
        {"a10g",600},{"a10",600},{"t4",320},{"v100",897},{"a6000",768},{"a5000",768},{"a4000",448},
        // AMD
        {"7900 xtx",960},{"7900 xt",800},{"7800 xt",624},{"7700 xt",432},{"7600",288},
        {"6900 xt",512},{"6800 xt",512},{"6800",512},{"6700 xt",384},{"9070 xt",624},{"9070",488},
        {"mi300x",5300},{"mi300",5300},{"mi250",3277},{"mi210",1638},{"mi100",1229},
        // Apple
        {"m5 max",614},{"m5 pro",307},{"m5",153.6},{"m4 ultra",819},{"m4 max",546},{"m4 pro",273},{"m4",120},
        {"m3 ultra",800},{"m3 max",400},{"m3 pro",150},{"m3",100},{"m2 ultra",800},{"m2 max",400},
        {"m2 pro",200},{"m2",100},{"m1 ultra",800},{"m1 max",400},{"m1 pro",200},{"m1",68},
    };
    for (const auto & e : table) if (n.find(e.key) != std::string::npos) return e.bw;
    return std::nullopt;
}

}  // namespace

// ===========================================================================
// catalogue
// ===========================================================================
struct ModelEntry {
    std::string name, provider, parameter_count, quantization, use_case,
                architecture, license, release_date;
    double      min_ram_gb = 0, recommended_ram_gb = 0;
    double      min_vram_gb = -1;       // <0 = absent
    std::uint64_t parameters_raw = 0;   bool has_params_raw = false;
    std::uint32_t context_length = 0;
    bool        is_moe = false;
    std::uint32_t num_experts = 0, active_experts = 0;  bool has_active_experts = false;
    std::uint64_t active_parameters = 0; bool has_active_params = false;
    std::uint32_t num_attention_heads = 0, num_key_value_heads = 0, num_hidden_layers = 0,
                  head_dim = 0, hidden_size = 0, moe_intermediate_size = 0, vocab_size = 0;
    std::vector<std::pair<std::string,std::string>> gguf_sources;  // repo, provider
    std::vector<std::string> capabilities;

    double params_b() const {
        if (has_params_raw && parameters_raw > 0) return (double) parameters_raw / 1e9;
        std::string u = to_upper(parameter_count);
        // trim
        while (!u.empty() && std::isspace((unsigned char) u.back())) u.pop_back();
        if (u.empty()) return 7.0;
        char last = u.back();
        try {
            double v = std::stod(u.substr(0, u.size() - 1));
            if (last == 'B') return v;
            if (last == 'M') return v / 1000.0;
            return std::stod(u);
        } catch (...) { return last == 'M' ? 0.0 : 7.0; }
    }
};

// A HuggingFace search hit (pre-enrichment) and an enriched, cached entry.
struct HfHit  { std::string repo, owner, pipeline; std::uint64_t downloads = 0, likes = 0; };
struct CachedHf { std::string repo; ModelEntry entry; std::uint64_t best_size = 0, downloads = 0, likes = 0; bool ok = false; };
struct ModelsEngine::HfCache { std::map<std::string, CachedHf> by_repo; };

namespace {

std::uint32_t jget_u32(const ordered_json & j, const char * k) {
    if (!j.contains(k) || j[k].is_null()) return 0;
    try { return j[k].get<std::uint32_t>(); } catch (...) { return 0; }
}
std::uint64_t jget_u64(const ordered_json & j, const char * k, bool & present) {
    present = j.contains(k) && !j[k].is_null();
    if (!present) return 0;
    try { return j[k].get<std::uint64_t>(); } catch (...) { present = false; return 0; }
}
double jget_d(const ordered_json & j, const char * k, double def) {
    if (!j.contains(k) || j[k].is_null()) return def;
    try { return j[k].get<double>(); } catch (...) { return def; }
}
std::string jget_s(const ordered_json & j, const char * k) {
    if (!j.contains(k) || j[k].is_null()) return "";
    try { return j[k].get<std::string>(); } catch (...) { return ""; }
}

// (The bundled-catalog loader was removed: the Recommend tab now sources
// models live from the HuggingFace listing API — see hf_search /
// hf_build_entry at the bottom of this file. jget_* are reused there.)

// ----- use-case classification (models.rs:439) -----
std::string infer_use_case(const ModelEntry & m) {
    std::string uc = to_lower(m.use_case), nm = to_lower(m.name);
    if (uc.find("embedding") != std::string::npos || nm.find("embed") != std::string::npos ||
        nm.find("bge") != std::string::npos) return "embedding";
    if (uc.find("code") != std::string::npos || nm.find("code") != std::string::npos) return "coding";
    if (uc.find("vision") != std::string::npos || uc.find("multimodal") != std::string::npos) return "multimodal";
    if (uc.find("reason") != std::string::npos || nm.find("deepseek-r1") != std::string::npos) return "reasoning";
    if (uc.find("chat") != std::string::npos || uc.find("instruction") != std::string::npos) return "chat";
    return "general";
}
std::string use_case_label(const std::string & code) {
    if (code == "coding") return "Coding";
    if (code == "reasoning") return "Reasoning";
    if (code == "chat") return "Chat";
    if (code == "multimodal") return "Multimodal";
    if (code == "embedding") return "Embedding";
    return "General";
}

// ===========================================================================
// hardware detection (hardware.rs)
// ===========================================================================
struct GpuInfo { std::string name; double vram_gb = -1; std::string backend; int count = 1; bool unified = false; };
struct SystemSpecs {
    double total_ram_gb = 0, available_ram_gb = 0;
    int    cpu_cores = 0;
    std::string cpu_name = "CPU";
    bool   has_gpu = false;
    double gpu_vram_gb = -1, total_gpu_vram_gb = -1;
    std::string gpu_name;
    int    gpu_count = 0;
    bool   unified_memory = false;
    std::string backend = "CPU";
    std::vector<GpuInfo> gpus;
    std::string node_name = "easyai", os_name =
#if defined(__APPLE__)
        "macos";
#elif defined(_WIN32)
        "windows";
#else
        "linux";
#endif
};

void detect_ram_cpu(SystemSpecs & s) {
    s.cpu_cores = (int) std::max(1u, std::thread::hardware_concurrency());
#if defined(__APPLE__)
    {
        int64_t mem = 0; size_t len = sizeof(mem);
        if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 && mem > 0)
            s.total_ram_gb = (double) mem / (1024.0 * 1024.0 * 1024.0);
        char brand[256]; len = sizeof(brand);
        if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0)
            s.cpu_name = brand;
        // available = (free + inactive) * page_size
        vm_size_t page = 0; host_page_size(mach_host_self(), &page);
        vm_statistics64_data_t vms; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vms, &cnt) == KERN_SUCCESS) {
            double freeb = (double)(vms.free_count + vms.inactive_count + vms.purgeable_count) * (double) page;
            s.available_ram_gb = freeb / (1024.0 * 1024.0 * 1024.0);
        }
    }
#else
    {
        std::ifstream mi("/proc/meminfo");
        std::string key; long val; std::string unit;
        double total_kb = 0, avail_kb = 0;
        while (mi >> key >> val >> unit) {
            if (key == "MemTotal:")     total_kb = (double) val;
            else if (key == "MemAvailable:") avail_kb = (double) val;
        }
        s.total_ram_gb     = total_kb / (1024.0 * 1024.0);
        s.available_ram_gb = avail_kb / (1024.0 * 1024.0);
        std::ifstream ci("/proc/cpuinfo"); std::string line;
        while (std::getline(ci, line)) {
            if (line.rfind("model name", 0) == 0) {
                auto c = line.find(':'); if (c != std::string::npos) s.cpu_name = line.substr(c + 2); break;
            }
        }
    }
#endif
    if (s.total_ram_gb <= 0) s.total_ram_gb = 8.0;
    if (s.available_ram_gb <= 0) s.available_ram_gb = s.total_ram_gb * 0.8;
}

std::string backend_label(ggml_backend_dev_t dev) {
    ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
    const char * n = reg ? ggml_backend_reg_name(reg) : nullptr;
    return n ? std::string(n) : std::string("GPU");
}

void detect_gpu(SystemSpecs & s) {
    static std::once_flag once;
    std::call_once(once, [] { ggml_backend_load_all(); });
    size_t n = ggml_backend_dev_count();
    GpuInfo best;
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (!dev) continue;
        enum ggml_backend_dev_type t = ggml_backend_dev_type(dev);
        if (t != GGML_BACKEND_DEVICE_TYPE_GPU && t != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        ggml_backend_dev_props props; std::memset(&props, 0, sizeof(props));
        ggml_backend_dev_get_props(dev, &props);
        GpuInfo g;
        g.name    = props.name ? props.name : "GPU";
        g.backend = backend_label(dev);
        g.vram_gb = props.memory_total > 0 ? (double) props.memory_total / (1024.0*1024.0*1024.0) : -1;
        g.unified = (t == GGML_BACKEND_DEVICE_TYPE_IGPU) ||
                    contains_ci(g.backend, "metal") || contains_ci(g.backend, "mtl") ||
                    contains_ci(g.name, "apple") || contains_ci(g.name, "mtl");
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        g.unified = true;   // Apple Silicon is always unified memory
#endif
        GpuInfo * cur = &g;
        s.gpus.push_back(g);
        if (best.vram_gb < 0 || (cur->vram_gb > best.vram_gb)) best = g;
    }
    if (!s.gpus.empty()) {
        s.has_gpu        = true;
        s.gpu_name       = best.name;
        s.unified_memory = best.unified;
        s.backend        = best.backend;
        s.gpu_count      = 1;
        // Apple unified memory: VRAM == system RAM.
        if (best.unified) { s.gpu_vram_gb = s.total_ram_gb; }
        else              { s.gpu_vram_gb = best.vram_gb;   }
        s.total_gpu_vram_gb = s.gpu_vram_gb;
        // Prettify ggml's terse Metal labels ("MTL"/"MTL0"). Using the CPU
        // brand as the GPU name on Apple unified silicon is both accurate and
        // lets the bandwidth table match ("Apple M3 Max" → 400 GB/s, etc.).
        if (s.backend == "MTL" || contains_ci(s.backend, "metal")) s.backend = "Metal";
        if (best.unified && (s.gpu_name.rfind("MTL", 0) == 0 || s.gpu_name.empty()) &&
            !s.cpu_name.empty()) s.gpu_name = s.cpu_name;
    } else {
#if defined(__aarch64__) || defined(__arm64__)
        s.backend = "CPU (ARM)";
#else
        s.backend = "CPU (x86)";
#endif
    }
}

SystemSpecs detect_system() {
    SystemSpecs s; detect_ram_cpu(s); detect_gpu(s); return s;
}

void apply_sim(SystemSpecs & s, double ram, double vram, int cores) {
    if (ram > 0) { s.total_ram_gb = ram; s.available_ram_gb = ram * 0.9;
        if (s.unified_memory) { s.gpu_vram_gb = ram; s.total_gpu_vram_gb = ram; } }
    if (vram >= 0) {
        s.has_gpu = true; s.gpu_vram_gb = vram; s.total_gpu_vram_gb = vram;
        if (s.gpu_name.empty()) s.gpu_name = "User-specified GPU";
        if (s.gpu_count == 0) s.gpu_count = 1;
        if (s.backend.rfind("CPU", 0) == 0) s.backend = "CUDA";
    }
    if (cores > 0) s.cpu_cores = cores;
}

// ===========================================================================
// memory model (models.rs)
// ===========================================================================
double kv_cache_gb(const ModelEntry & m, std::uint32_t ctx, const std::string & kv_quant) {
    double bpe = kv_bytes_per_elem(kv_quant);
    if (m.num_hidden_layers > 0 && m.head_dim > 0) {
        double n_kv = m.num_key_value_heads ? m.num_key_value_heads
                    : (m.num_attention_heads ? m.num_attention_heads : 8);
        double per_layer = 2.0 * n_kv * (double) m.head_dim * (double) ctx * bpe;
        double total = per_layer * (double) m.num_hidden_layers;
        return total / 1073741824.0;
    }
    // fallback: coarse fp16 baseline scaled by quant
    double baseline = 0.000008 * m.params_b() * (double) ctx;
    double scale = 1.0;
    std::string u = to_upper(kv_quant);
    if (u == "FP8" || u == "Q8_0") scale = 0.5;
    else if (u == "Q4_0") scale = 0.25;
    return baseline * scale;
}

double estimate_memory_gb(const ModelEntry & m, const std::string & quant,
                          std::uint32_t ctx, const std::string & kv_quant = "fp16") {
    return m.params_b() * quant_bpp(quant) + kv_cache_gb(m, ctx, kv_quant) + 0.5;
}

// best quant fitting `budget` GB (models.rs:885)
bool best_quant_for_budget(const ModelEntry & m, double budget, std::uint32_t ctx,
                           std::string & out_quant, double & out_mem) {
    for (const auto & q : kQuantHierarchy) {
        double mem = estimate_memory_gb(m, q, ctx);
        if (mem <= budget) { out_quant = q; out_mem = mem; return true; }
    }
    std::uint32_t half = ctx / 2;
    if (half >= 1024) {
        for (const auto & q : kQuantHierarchy) {
            double mem = estimate_memory_gb(m, q, half);
            if (mem <= budget) { out_quant = q; out_mem = mem; return true; }
        }
    }
    return false;
}

// ===========================================================================
// fit result + scoring (fit.rs)
// ===========================================================================
struct FitRow {
    const ModelEntry * m = nullptr;
    std::string run_mode = "cpu_only";       // gpu|tensor_parallel|moe_offload|cpu_offload|cpu_only
    std::string fit_level = "too_tight";     // perfect|good|marginal|too_tight
    std::string runtime = "llamacpp";        // mlx|llamacpp|vllm
    std::string best_quant;
    double memory_required_gb = 0, memory_available_gb = 0;
    double moe_offloaded_gb = -1;            // <0 = none
    double utilization_pct = 0;
    double estimated_tps = 0;
    double score = 0, q_quality = 0, q_speed = 0, q_fit = 0, q_context = 0;
    std::uint32_t est_ctx = 0;
    std::vector<std::string> notes;
    // HuggingFace popularity / file size (live-source rows only).
    std::uint64_t hf_downloads = 0, hf_likes = 0, file_size = 0;
};

std::string run_mode_label(const std::string & c) {
    if (c == "gpu") return "GPU";
    if (c == "tensor_parallel") return "Tensor Parallel";
    if (c == "moe_offload") return "MoE Offload";
    if (c == "cpu_offload") return "CPU Offload";
    return "CPU";
}
std::string fit_label(const std::string & c) {
    if (c == "perfect") return "Perfect fit";
    if (c == "good") return "Good fit";
    if (c == "marginal") return "Marginal";
    return "Too tight";
}

std::string select_runtime(const SystemSpecs & s, const std::string & force) {
    if (force == "mlx" || force == "llamacpp" || force == "vllm") return force;
    if (contains_ci(s.backend, "metal") && s.unified_memory) return "mlx";
    return "llamacpp";
}

double estimate_tps(const ModelEntry & m, const std::string & quant,
                    const std::string & run_mode, const SystemSpecs & s) {
    const double efficiency = 0.55;
    double params = (m.is_moe && m.has_active_params)
                  ? (double) m.active_parameters / 1e9 : m.params_b();
    params = std::max(params, 0.1);
    double mode_factor = run_mode == "gpu" ? 1.0 : run_mode == "tensor_parallel" ? 0.9
                       : run_mode == "moe_offload" ? 0.8 : run_mode == "cpu_offload" ? 0.5 : 0.3;

    auto bw = (run_mode != "cpu_only" && s.has_gpu) ? gpu_bandwidth_gbps(s.gpu_name) : std::nullopt;
    if (bw) {
        double active_gb = params * quant_bytes_per_param(quant);
        if (active_gb <= 0) active_gb = 0.1;
        if (m.is_moe && run_mode == "moe_offload") {
            double ddr = 50.0;
            double t = active_gb / ddr + active_gb / (*bw * efficiency);
            return std::max((1.0 / t) * 0.8, 0.1);
        }
        if (m.is_moe) {  // Gpu MoE — Tier-2 overhead by expert count
            double overhead = 0.60;
            if (m.num_experts) overhead = m.num_experts <= 8 ? 0.90 : m.num_experts <= 16 ? 0.85
                              : m.num_experts <= 32 ? 0.80 : m.num_experts <= 64 ? 0.70 : 0.40;
            double moe_active = params * quant_bpp(quant);
            return std::max((*bw / moe_active) * efficiency * overhead * mode_factor, 0.1);
        }
        return std::max((*bw / active_gb) * efficiency * mode_factor, 0.1);
    }
    // fixed-K fallback
    double k = 70.0;
    if (contains_ci(s.backend, "metal")) k = 160.0;
    else if (contains_ci(s.backend, "cuda")) k = 220.0;
    else if (contains_ci(s.backend, "rocm") || contains_ci(s.backend, "hip")) k = 180.0;
    else if (contains_ci(s.backend, "vulkan")) k = 150.0;
    else if (contains_ci(s.backend, "sycl")) k = 100.0;
#if defined(__aarch64__) || defined(__arm64__)
    else k = 90.0;
#endif
    double base = (k / params) * quant_speed_mult(quant);
    if (s.cpu_cores >= 8) base *= 1.1;
    if (run_mode == "cpu_only") {
#if defined(__aarch64__) || defined(__arm64__)
        double cpu_k = 90.0;
#else
        double cpu_k = 70.0;
#endif
        base = (cpu_k / params) * quant_speed_mult(quant);
        if (s.cpu_cores >= 8) base *= 1.1;
    }
    base *= mode_factor;
    return std::max(base, 0.1);
}

double quality_score(const ModelEntry & m, const std::string & quant, const std::string & uc) {
    double p = m.params_b();
    double base = p < 1 ? 30 : p < 3 ? 45 : p < 7 ? 60 : p < 10 ? 75 : p < 20 ? 82 : p < 40 ? 89 : 95;
    std::string nm = to_lower(m.name);
    double fam = 0;
    if (nm.find("qwen") != std::string::npos) fam = 2;
    else if (nm.find("deepseek") != std::string::npos) fam = 3;
    else if (nm.find("llama") != std::string::npos) fam = 2;
    else if (nm.find("mistral") != std::string::npos || nm.find("mixtral") != std::string::npos) fam = 1;
    else if (nm.find("gemma") != std::string::npos) fam = 1;
    else if (nm.find("starcoder") != std::string::npos) fam = 1;
    double qp = quant_quality_penalty(quant);
    double task = 0;
    if (uc == "coding" && (nm.find("code") != std::string::npos || nm.find("starcoder") != std::string::npos
        || nm.find("wizard") != std::string::npos)) task = 6;
    else if (uc == "reasoning" && p >= 13) task = 5;
    else if (uc == "multimodal" && (nm.find("vision") != std::string::npos ||
             contains_ci(m.use_case, "vision"))) task = 6;
    return clampd(base + fam + qp + task, 0, 100);
}
double speed_score(double tps, const std::string & uc) {
    double target = uc == "reasoning" ? 25.0 : uc == "embedding" ? 200.0 : 40.0;
    return clampd((tps / target) * 100.0, 0, 100);
}
double fit_component_score(double req, double avail) {
    if (avail <= 0 || req > avail) return 0.0;
    double r = req / avail;
    if (r <= 0.5) return 60.0 + (r / 0.5) * 40.0;
    if (r <= 0.8) return 100.0;
    if (r <= 0.9) return 70.0;
    return 50.0;
}
double context_score(std::uint32_t ctx, const std::string & uc) {
    if (ctx == 0) return 70.0;   // unknown (live HF rows) — neutral, don't tank the score
    std::uint32_t target = (uc == "coding" || uc == "reasoning") ? 8192 : uc == "embedding" ? 512 : 4096;
    if (ctx >= target) return 100.0;
    if (ctx >= target / 2) return 70.0;
    return 30.0;
}
void weights_for(const std::string & uc, double & wq, double & ws, double & wf, double & wc) {
    if (uc == "coding")        { wq=0.50; ws=0.20; wf=0.15; wc=0.15; }
    else if (uc == "reasoning"){ wq=0.55; ws=0.15; wf=0.15; wc=0.15; }
    else if (uc == "chat")     { wq=0.40; ws=0.35; wf=0.15; wc=0.10; }
    else if (uc == "multimodal"){wq=0.50; ws=0.20; wf=0.15; wc=0.15; }
    else if (uc == "embedding"){ wq=0.30; ws=0.40; wf=0.20; wc=0.10; }
    else                       { wq=0.45; ws=0.30; wf=0.15; wc=0.10; }
}

// MoE offload helpers (fit.rs:747)
double moe_active_vram(const ModelEntry & m, const std::string & quant) {
    double active = m.has_active_params ? (double) m.active_parameters : m.params_b() * 1e9 * 0.2;
    return std::max((active * quant_bpp(quant)) / 1073741824.0 * 1.1, 0.5);
}
double moe_offloaded_ram(const ModelEntry & m, const std::string & quant) {
    double total = m.has_params_raw ? (double) m.parameters_raw : m.params_b() * 1e9;
    double active = m.has_active_params ? (double) m.active_parameters : total * 0.2;
    double inactive = std::max(total - active, 0.0);
    return (inactive * quant_bpp(quant)) / 1073741824.0;
}

// The core: produce a FitRow for one model on `s`.
FitRow score_model(const ModelEntry & m, const SystemSpecs & s,
                   std::uint32_t ctx_cap, const std::string & force_runtime) {
    FitRow r; r.m = &m;
    std::uint32_t est_ctx = m.context_length ? m.context_length : 8192;
    std::uint32_t cap = ctx_cap > 0 ? ctx_cap : 8192;
    est_ctx = std::min(est_ctx, cap);
    if (est_ctx == 0) est_ctx = 4096;
    r.est_ctx = est_ctx;
    r.runtime = select_runtime(s, force_runtime);

    double min_vram = m.min_vram_gb >= 0 ? m.min_vram_gb : m.min_ram_gb;
    std::string default_quant = m.quantization;
    double default_mem = estimate_memory_gb(m, default_quant, est_ctx);
    r.best_quant = default_quant;
    r.memory_required_gb = default_mem;

    auto try_budget = [&](double budget, std::string & bq, double & bm) {
        return best_quant_for_budget(m, budget, est_ctx, bq, bm);
    };

    if (s.has_gpu && s.gpu_vram_gb > 0) {
        if (s.unified_memory) {
            double pool = s.gpu_vram_gb;
            r.run_mode = "gpu"; r.memory_available_gb = pool;
            if (m.is_moe) { r.memory_required_gb = std::max(min_vram, 0.5); }
            else { std::string bq; double bm; if (try_budget(pool, bq, bm)) { r.best_quant = bq; r.memory_required_gb = bm; } }
        } else {
            double sys_vram = s.total_gpu_vram_gb > 0 ? s.total_gpu_vram_gb : s.gpu_vram_gb;
            if (m.is_moe && min_vram <= sys_vram) {
                r.run_mode = "gpu"; r.memory_required_gb = min_vram; r.memory_available_gb = sys_vram;
            } else if (m.is_moe) {
                // try fully on GPU, else MoE offload
                std::string bq; double bm;
                if (try_budget(sys_vram, bq, bm)) { r.run_mode = "gpu"; r.best_quant = bq; r.memory_required_gb = bm; r.memory_available_gb = sys_vram; }
                else {
                    double mv = moe_active_vram(m, default_quant), off = moe_offloaded_ram(m, default_quant);
                    if (mv <= sys_vram && off <= s.available_ram_gb) {
                        r.run_mode = "moe_offload"; r.memory_required_gb = mv; r.memory_available_gb = sys_vram; r.moe_offloaded_gb = off;
                    } else if (m.min_ram_gb <= s.available_ram_gb) {
                        r.run_mode = "cpu_offload"; r.memory_required_gb = m.min_ram_gb; r.memory_available_gb = s.available_ram_gb;
                    } else { r.run_mode = "gpu"; r.memory_required_gb = default_mem; r.memory_available_gb = sys_vram; }
                }
            } else {
                std::string bq; double bm;
                if (try_budget(sys_vram, bq, bm)) { r.run_mode = "gpu"; r.best_quant = bq; r.memory_required_gb = bm; r.memory_available_gb = sys_vram; }
                else if (try_budget(s.available_ram_gb, bq, bm)) { r.run_mode = "cpu_offload"; r.best_quant = bq; r.memory_required_gb = bm; r.memory_available_gb = s.available_ram_gb; }
                else { r.run_mode = "gpu"; r.memory_required_gb = default_mem; r.memory_available_gb = sys_vram; }
            }
        }
    } else {
        r.memory_available_gb = s.available_ram_gb;
        if (m.is_moe) { r.run_mode = "cpu_only"; r.memory_required_gb = m.min_ram_gb > 0 ? m.min_ram_gb : default_mem; }
        else {
            std::string bq; double bm;
            if (try_budget(s.available_ram_gb, bq, bm)) { r.run_mode = "cpu_only"; r.best_quant = bq; r.memory_required_gb = bm; }
            else { r.run_mode = "cpu_only"; r.memory_required_gb = default_mem; }
        }
    }

    if (r.best_quant != default_quant)
        r.notes.push_back("Best quant for hardware: " + r.best_quant + " (model default: " + default_quant + ")");

    // utilization + fit level
    r.utilization_pct = r.memory_available_gb > 0 ? r.memory_required_gb / r.memory_available_gb * 100.0 : 1e9;
    if (r.memory_required_gb > r.memory_available_gb) r.fit_level = "too_tight";
    else if (r.run_mode == "gpu" || r.run_mode == "tensor_parallel") {
        if (m.recommended_ram_gb > 0 && m.recommended_ram_gb <= r.memory_available_gb) r.fit_level = "perfect";
        else if (r.memory_available_gb >= r.memory_required_gb * 1.2) r.fit_level = "good";
        else r.fit_level = "marginal";
    } else if (r.run_mode == "moe_offload" || r.run_mode == "cpu_offload") {
        r.fit_level = r.memory_available_gb >= r.memory_required_gb * 1.2 ? "good" : "marginal";
    } else r.fit_level = "marginal";  // cpu_only capped

    r.estimated_tps = estimate_tps(m, r.best_quant, r.run_mode, s);

    std::string uc = infer_use_case(m);
    r.q_quality = quality_score(m, r.best_quant, uc);
    r.q_speed   = speed_score(r.estimated_tps, uc);
    r.q_fit     = fit_component_score(r.memory_required_gb, r.memory_available_gb);
    r.q_context = context_score(m.context_length, uc);
    double wq, ws, wf, wc; weights_for(uc, wq, ws, wf, wc);
    r.score = round1(r.q_quality * wq + r.q_speed * ws + r.q_fit * wf + r.q_context * wc);
    return r;
}

int fit_rank(const std::string & f) {
    return f == "perfect" ? 4 : f == "good" ? 3 : f == "marginal" ? 2 : 1;
}

// ----- JSON emitters -----
ordered_json system_to_json(const SystemSpecs & s) {
    ordered_json gpus = ordered_json::array();
    for (const auto & g : s.gpus)
        gpus.push_back({ {"name", g.name}, {"vram_gb", g.vram_gb >= 0 ? ordered_json(round2(g.vram_gb)) : ordered_json(nullptr)},
                         {"backend", g.backend}, {"count", g.count}, {"unified_memory", g.unified} });
    return {
        {"node", { {"name", s.node_name}, {"os", s.os_name} }},
        {"system", {
            {"total_ram_gb", round2(s.total_ram_gb)},
            {"available_ram_gb", round2(s.available_ram_gb)},
            {"cpu_cores", s.cpu_cores}, {"cpu_name", s.cpu_name},
            {"has_gpu", s.has_gpu},
            {"gpu_vram_gb", s.gpu_vram_gb >= 0 ? ordered_json(round2(s.gpu_vram_gb)) : ordered_json(nullptr)},
            {"gpu_name", s.gpu_name.empty() ? ordered_json(nullptr) : ordered_json(s.gpu_name)},
            {"gpu_count", s.gpu_count}, {"unified_memory", s.unified_memory},
            {"backend", s.backend}, {"gpus", gpus},
        }},
    };
}

ordered_json fit_to_json(const FitRow & r) {
    const ModelEntry & m = *r.m;
    std::string uc = infer_use_case(m);
    ordered_json srcs = ordered_json::array();
    for (const auto & g : m.gguf_sources) srcs.push_back({ {"repo", g.first}, {"provider", g.second} });
    return {
        {"name", m.name}, {"provider", m.provider},
        {"parameter_count", m.parameter_count}, {"params_b", round2(m.params_b())},
        {"context_length", m.context_length}, {"use_case", m.use_case},
        {"category", use_case_label(uc)},
        {"release_date", m.release_date.empty() ? ordered_json(nullptr) : ordered_json(m.release_date)},
        {"is_moe", m.is_moe},
        {"fit_level", r.fit_level}, {"fit_label", fit_label(r.fit_level)},
        {"run_mode", r.run_mode}, {"run_mode_label", run_mode_label(r.run_mode)},
        {"score", round1(r.score)},
        {"score_components", { {"quality", round1(r.q_quality)}, {"speed", round1(r.q_speed)},
                               {"fit", round1(r.q_fit)}, {"context", round1(r.q_context)} }},
        {"estimated_tps", round1(r.estimated_tps)},
        {"runtime", r.runtime}, {"runtime_label", r.runtime == "mlx" ? "MLX" : r.runtime == "vllm" ? "vLLM" : "llama.cpp"},
        {"best_quant", r.best_quant.empty() ? ordered_json(nullptr) : ordered_json(r.best_quant)},
        {"memory_required_gb", round2(r.memory_required_gb)},
        {"memory_available_gb", round2(r.memory_available_gb)},
        {"moe_offloaded_gb", r.moe_offloaded_gb >= 0 ? ordered_json(round2(r.moe_offloaded_gb)) : ordered_json(nullptr)},
        {"total_memory_gb", round2(r.memory_required_gb + (r.moe_offloaded_gb >= 0 ? r.moe_offloaded_gb : 0))},
        {"utilization_pct", round1(r.utilization_pct)},
        {"notes", r.notes},
        {"gguf_sources", srcs},
        {"capabilities", m.capabilities},
        {"license", m.license.empty() ? ordered_json(nullptr) : ordered_json(m.license)},
        {"hf_downloads", r.hf_downloads}, {"hf_likes", r.hf_likes},
        {"file_size_bytes", r.file_size},
    };
}

// ===========================================================================
// GGUF introspection (local model parameters)
// ===========================================================================
struct GgufMeta {
    bool ok = false;
    std::string arch, name, quant;
    std::uint32_t context_length = 0, n_layers = 0, n_heads = 0, n_kv_heads = 0,
                  head_dim = 0, embd = 0, expert_count = 0, expert_used = 0, vocab = 0;
    std::uint64_t parameters = 0; bool has_params = false;
    std::uint64_t file_size = 0;
    std::map<std::string, std::string> extra;   // a few human-readable extras
};

std::string ggval_to_string(const gguf_context * c, int64_t id) {
    switch (gguf_get_kv_type(c, id)) {
        case GGUF_TYPE_UINT8:  return std::to_string(gguf_get_val_u8(c, id));
        case GGUF_TYPE_INT8:   return std::to_string(gguf_get_val_i8(c, id));
        case GGUF_TYPE_UINT16: return std::to_string(gguf_get_val_u16(c, id));
        case GGUF_TYPE_INT16:  return std::to_string(gguf_get_val_i16(c, id));
        case GGUF_TYPE_UINT32: return std::to_string(gguf_get_val_u32(c, id));
        case GGUF_TYPE_INT32:  return std::to_string(gguf_get_val_i32(c, id));
        case GGUF_TYPE_FLOAT32:return std::to_string(gguf_get_val_f32(c, id));
        case GGUF_TYPE_UINT64: return std::to_string(gguf_get_val_u64(c, id));
        case GGUF_TYPE_INT64:  return std::to_string(gguf_get_val_i64(c, id));
        case GGUF_TYPE_FLOAT64:return std::to_string(gguf_get_val_f64(c, id));
        case GGUF_TYPE_BOOL:   return gguf_get_val_bool(c, id) ? "true" : "false";
        case GGUF_TYPE_STRING: return gguf_get_val_str(c, id);
        default: return "";
    }
}
std::uint64_t ggval_u64(const gguf_context * c, const std::string & key, bool & found) {
    int64_t id = gguf_find_key(c, key.c_str());
    found = id >= 0;
    if (!found) return 0;
    switch (gguf_get_kv_type(c, id)) {
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(c, id);
        case GGUF_TYPE_INT32:  return (std::uint64_t) gguf_get_val_i32(c, id);
        case GGUF_TYPE_UINT64: return gguf_get_val_u64(c, id);
        case GGUF_TYPE_INT64:  return (std::uint64_t) gguf_get_val_i64(c, id);
        default: found = false; return 0;
    }
}

// llama_ftype → quant string (common values)
std::string ftype_to_quant(std::uint32_t ft) {
    switch (ft) {
        case 0: return "F32";   case 1: return "F16";  case 2: return "Q4_0"; case 3: return "Q4_1";
        case 7: return "Q8_0";  case 8: return "Q5_0"; case 9: return "Q5_1"; case 10: return "Q2_K";
        case 11: return "Q3_K_S"; case 12: return "Q3_K_M"; case 13: return "Q3_K_L";
        case 14: return "Q4_K_S"; case 15: return "Q4_K_M"; case 16: return "Q5_K_S";
        case 17: return "Q5_K_M"; case 18: return "Q6_K";  case 32: return "BF16";
        default: return "";
    }
}

GgufMeta read_gguf_meta(const std::string & path) {
    GgufMeta g;
    std::error_code ec; g.file_size = (std::uint64_t) fs::file_size(path, ec);
    gguf_init_params p; p.no_alloc = true; p.ctx = nullptr;
    gguf_context * c = gguf_init_from_file(path.c_str(), p);
    if (!c) return g;
    g.ok = true;
    auto sval = [&](const char * k) -> std::string {
        int64_t id = gguf_find_key(c, k);
        return id >= 0 ? ggval_to_string(c, id) : std::string();
    };
    g.arch = sval("general.architecture");
    g.name = sval("general.name");
    bool f;
    std::uint64_t ft = ggval_u64(c, "general.file_type", f);
    if (f) g.quant = ftype_to_quant((std::uint32_t) ft);
    // filename-derived quant is more reliable; caller may override.
    auto a = [&](const std::string & suf) { return g.arch.empty() ? suf : g.arch + "." + suf; };
    g.context_length = (std::uint32_t) ggval_u64(c, a("context_length"), f);
    g.n_layers       = (std::uint32_t) ggval_u64(c, a("block_count"), f);
    g.n_heads        = (std::uint32_t) ggval_u64(c, a("attention.head_count"), f);
    g.n_kv_heads     = (std::uint32_t) ggval_u64(c, a("attention.head_count_kv"), f);
    g.embd           = (std::uint32_t) ggval_u64(c, a("embedding_length"), f);
    g.head_dim       = (std::uint32_t) ggval_u64(c, a("attention.key_length"), f);
    if (g.head_dim == 0 && g.n_heads > 0 && g.embd > 0) g.head_dim = g.embd / g.n_heads;
    g.expert_count   = (std::uint32_t) ggval_u64(c, a("expert_count"), f);
    g.expert_used    = (std::uint32_t) ggval_u64(c, a("expert_used_count"), f);
    g.vocab          = (std::uint32_t) ggval_u64(c, a("vocab_size"), f);
    g.parameters     = ggval_u64(c, "general.parameter_count", g.has_params);
    // a couple of human extras
    std::string org = sval("general.organization");
    if (!org.empty()) g.extra["organization"] = org;
    std::string ft_name = sval("general.file_type");
    gguf_free(c);
    return g;
}

}  // namespace

// ===========================================================================
// ModelsEngine
// ===========================================================================
ModelsEngine::ModelsEngine(std::string download_dir, const config::Ini * ini,
                           std::function<std::string()> current_model)
    : download_dir_(std::move(download_dir)),
      ini_(ini),
      current_model_(std::move(current_model)),
      hf_cache_(std::make_unique<HfCache>()) {
    status_ = "live HuggingFace catalog";
}
ModelsEngine::~ModelsEngine() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

std::string ModelsEngine::status_message() const { return status_; }

std::string ModelsEngine::system_json(const std::string & query) {
    auto q = parse_query(query);
    SystemSpecs s = detect_system();
    apply_sim(s, qnum(q, "ram_gb", qnum(q, "ram", -1)), qnum(q, "vram_gb", qnum(q, "memory", -1)),
              (int) qnum(q, "cpu_cores", -1));
    return system_to_json(s).dump();
}

// models_json (live HuggingFace search) and plan_json (live HF, by repo id)
// are defined at the BOTTOM of this file — after the curl + GGUF + HF helpers
// they depend on.

// ----- local models -----
namespace {
std::string base_name(const std::string & path) {
    size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
bool safe_in_dir(const std::string & dir, const std::string & filename, fs::path & out, std::string & err) {
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos || filename.find("..") != std::string::npos) {
        err = "invalid filename"; return false;
    }
    if (!ends_with_ci(filename, ".gguf")) { err = "not a .gguf file"; return false; }
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    fs::path cand = fs::weakly_canonical(root / filename, ec);
    if (ec) { err = "path resolution failed"; return false; }
    auto a = root.begin(); auto b = cand.begin();
    for (; a != root.end(); ++a, ++b) if (b == cand.end() || *b != *a) { err = "path escapes download dir"; return false; }
    out = cand; return true;
}
}  // namespace

std::vector<ModelsEngine::LocalModel> ModelsEngine::list_local() {
    std::vector<LocalModel> out;
    std::error_code ec;
    if (download_dir_.empty() || !fs::is_directory(download_dir_, ec)) return out;
    std::string cur = current_model_ ? current_model_() : "";
    std::error_code cec; fs::path curp = cur.empty() ? fs::path() : fs::weakly_canonical(cur, cec);
    for (const auto & e : fs::directory_iterator(download_dir_, ec)) {
        if (ec) break;
        std::error_code fe;
        if (!e.is_regular_file(fe)) continue;
        std::string name = e.path().filename().string();
        if (!ends_with_ci(name, ".gguf")) continue;
        LocalModel m; m.name = name;
        m.size_bytes = (std::uint64_t) fs::file_size(e.path(), fe); if (fe) m.size_bytes = 0;
        auto wt = fs::last_write_time(e.path(), fe);
        if (!fe) {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                wt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            m.mtime = (std::int64_t) std::chrono::system_clock::to_time_t(sctp);
        }
        std::error_code rec; fs::path rp = fs::weakly_canonical(e.path(), rec);
        m.is_current = !curp.empty() && rp == curp;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const LocalModel & a, const LocalModel & b) { return a.name < b.name; });
    return out;
}

std::string ModelsEngine::local_models_json() {
    auto models = list_local();
    ordered_json arr = ordered_json::array();
    for (const auto & m : models)
        arr.push_back({ {"name", m.name}, {"size_bytes", m.size_bytes}, {"mtime", m.mtime}, {"is_current", m.is_current} });
    return ordered_json({ {"dir", download_dir_}, {"models", arr} }).dump();
}

std::string ModelsEngine::local_model_json(const std::string & filename, const std::string & query) {
    fs::path path; std::string err;
    if (!safe_in_dir(download_dir_, filename, path, err)) return "{\"error\":\"" + err + "\"}";
    std::error_code ec;
    if (!fs::exists(path, ec)) return "{\"error\":\"not found\"}";

    auto q = parse_query(query);
    SystemSpecs s = detect_system();
    apply_sim(s, qnum(q, "ram_gb", qnum(q, "ram", -1)), qnum(q, "vram_gb", qnum(q, "memory", -1)),
              (int) qnum(q, "cpu_cores", -1));

    GgufMeta g = read_gguf_meta(path.string());

    // Build a ModelEntry from GGUF metadata so we can reuse the fit scorer.
    ModelEntry m;
    m.name = g.name.empty() ? filename : g.name;
    m.architecture = g.arch;
    // quant: prefer the filename token, else GGUF file_type.
    std::string fquant;
    {
        static const char * order[] = { "Q8_0","Q6_K_L","Q6_K","Q5_K_M","Q5_K_S","Q5_0","Q4_K_M","Q4_K_S","Q4_0",
            "Q3_K_L","Q3_K_M","Q3_K_S","Q2_K","IQ4_XS","IQ3_M","IQ2_M","IQ1_M","F16","BF16","F32" };
        std::string uc = to_upper(filename);
        for (auto * o : order) if (uc.find(o) != std::string::npos) { fquant = o; break; }
    }
    m.quantization = !fquant.empty() ? fquant : (g.quant.empty() ? "Q4_K_M" : g.quant);
    m.context_length = g.context_length;
    m.num_hidden_layers = g.n_layers; m.num_attention_heads = g.n_heads;
    m.num_key_value_heads = g.n_kv_heads; m.head_dim = g.head_dim; m.hidden_size = g.embd;
    m.vocab_size = g.vocab;
    m.is_moe = g.expert_count > 1; m.num_experts = g.expert_count; m.active_experts = g.expert_used;
    m.has_active_experts = g.expert_used > 0;
    if (g.has_params) { m.parameters_raw = g.parameters; m.has_params_raw = true; }
    else if (g.file_size > 0) {
        // estimate params from on-disk size / bytes-per-weight
        m.parameters_raw = (std::uint64_t)((double) g.file_size / quant_bpp(m.quantization));
        m.has_params_raw = true;
    }
    double pb = m.params_b();
    { char pcbuf[32];
      if (pb >= 1) std::snprintf(pcbuf, sizeof(pcbuf), "%.1fB", pb);
      else         std::snprintf(pcbuf, sizeof(pcbuf), "%dM", (int) std::round(pb * 1000));
      m.parameter_count = pcbuf; }
    // rough RAM hints for fit thresholds
    m.min_ram_gb = estimate_memory_gb(m, m.quantization, m.context_length ? std::min<std::uint32_t>(m.context_length, 8192) : 4096);
    m.recommended_ram_gb = m.min_ram_gb * 1.25;

    FitRow r = score_model(m, s, 0, "");

    // matching [MODEL_*] INI profile
    ordered_json profile = ordered_json(nullptr);
    if (ini_) {
        std::string stem = filename;
        auto dot = stem.find_last_of('.'); if (dot != std::string::npos) stem = stem.substr(0, dot);
        std::string section = config::find_model_section(*ini_, stem);
        if (!section.empty()) {
            ordered_json kv = ordered_json::object();
            for (const auto & kvp : ini_->section_or_empty(section)) kv[kvp.first] = kvp.second;
            profile = { {"section", section}, {"values", kv} };
        }
    }

    std::string cur = current_model_ ? current_model_() : "";
    std::error_code cec; fs::path curp = cur.empty() ? fs::path() : fs::weakly_canonical(cur, cec);
    std::error_code rec2; fs::path rp = fs::weakly_canonical(path, rec2);
    bool is_current = !curp.empty() && rp == curp;

    ordered_json params = {
        {"architecture", g.arch}, {"quantization", m.quantization},
        {"params_b", round2(pb)}, {"parameter_count", m.parameter_count},
        {"context_length", g.context_length},
        {"num_hidden_layers", g.n_layers}, {"num_attention_heads", g.n_heads},
        {"num_key_value_heads", g.n_kv_heads}, {"head_dim", g.head_dim},
        {"embedding_length", g.embd}, {"vocab_size", g.vocab},
        {"expert_count", g.expert_count}, {"expert_used_count", g.expert_used},
        {"is_moe", m.is_moe}, {"file_size_bytes", g.file_size},
    };
    ordered_json out = {
        {"name", filename}, {"display_name", m.name}, {"is_current", is_current},
        {"gguf_readable", g.ok},
        {"params", params},
        {"fit", fit_to_json(r)},
        {"system", system_to_json(s)["system"]},
        {"ini_profile", profile},
    };
    return out.dump();
}

bool ModelsEngine::delete_local(const std::string & name, std::string & err) {
    fs::path dest;
    if (!safe_in_dir(download_dir_, name, dest, err)) return false;
    std::error_code ec;
    if (!fs::exists(dest, ec)) { err = "not found: " + name; return false; }
    if (!fs::is_regular_file(dest, ec)) { err = "not a regular file"; return false; }
    if (!fs::remove(dest, ec) || ec) { err = "delete failed: " + (ec ? ec.message() : std::string("unknown")); return false; }
    easyai::log::write("[models] deleted %s\n", name.c_str());
    return true;
}

bool ModelsEngine::resolve_local(const std::string & name, std::string & abs_path, std::string & err) {
    fs::path dest;
    if (!safe_in_dir(download_dir_, name, dest, err)) return false;
    std::error_code ec;
    if (!fs::exists(dest, ec) || !fs::is_regular_file(dest, ec)) { err = "not found: " + name; return false; }
    abs_path = dest.string();
    return true;
}

// ===========================================================================
// download manager (libcurl → HuggingFace) — native, single active download
// ===========================================================================
namespace {
std::string family_base(const std::string & path) {
    static const std::regex shard(R"(^(.*)-\d{5}-of-\d{5}\.gguf$)", std::regex::icase);
    std::smatch mm;
    if (std::regex_match(path, mm, shard)) return mm[1].str();
    if (ends_with_ci(path, ".gguf")) return path.substr(0, path.size() - 5);
    return path;
}
int quant_rank(const std::string & filename) {
    static const char * order[] = { "Q8_0","Q6_K_L","Q6_K","Q5_K_M","Q5_K_S","Q5_1","Q5_0",
        "Q4_K_M","Q4_K_S","Q4_1","Q4_0","Q3_K_L","Q3_K_M","Q3_K_S","Q3_K","Q2_K_L","Q2_K",
        "IQ4_XS","IQ4_NL","IQ3_M","IQ3_S","IQ2_M","IQ2_S","IQ1_M","IQ1_S","BF16","F16","F32" };
    std::string uc = to_upper(filename);
    int n = (int)(sizeof(order)/sizeof(order[0]));
    for (int i = 0; i < n; ++i) if (uc.find(order[i]) != std::string::npos) return i;
    return n + 1;
}

#if defined(EASYAI_HAVE_CURL)
void ensure_curl_global() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}
constexpr std::size_t kMaxApiBytes = 8u * 1024u * 1024u;
size_t str_write_cb(void * buf, size_t sz, size_t n, void * ud) {
    auto * out = static_cast<std::string *>(ud);
    size_t inc = sz * n;
    if (out->size() + inc > kMaxApiBytes) return 0;
    out->append(static_cast<char *>(buf), inc);
    return inc;
}
void apply_common_curl(CURL * c) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "easyai-models/1.0");
#if defined(LIBCURL_VERSION_NUM) && LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
}
bool curl_get_string(const std::string & url, std::string & out, std::string & err) {
    ensure_curl_global();
    CURL * c = curl_easy_init();
    if (!c) { err = "curl_easy_init failed"; return false; }
    out.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, str_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    apply_common_curl(c);
    CURLcode rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { err = std::string("curl: ") + curl_easy_strerror(rc); return false; }
    if (code >= 400) { err = "HTTP " + std::to_string(code); return false; }
    return true;
}
size_t raw_file_write(char * buf, size_t sz, size_t n, void * ud) {
    return std::fwrite(buf, 1, sz * n, static_cast<std::FILE *>(ud));
}
// Stream a URL straight to a file (no size cap; used for the catalog refresh,
// which is several MB — too large for the in-memory string helper).
bool curl_download_file(const std::string & url, const std::string & dest, std::string & err) {
    ensure_curl_global();
    std::FILE * fp = std::fopen(dest.c_str(), "wb");
    if (!fp) { err = "cannot open " + dest; return false; }
    CURL * c = curl_easy_init();
    if (!c) { std::fclose(fp); err = "curl_easy_init failed"; return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, raw_file_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    apply_common_curl(c);
    CURLcode rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c); std::fclose(fp);
    if (rc != CURLE_OK) { std::remove(dest.c_str()); err = std::string("curl: ") + curl_easy_strerror(rc); return false; }
    if (code >= 400)    { std::remove(dest.c_str()); err = "HTTP " + std::to_string(code); return false; }
    return true;
}
struct DlCb {
    std::FILE * fp = nullptr; std::mutex * mu = nullptr;
    ModelsEngine::DownloadStatus * st = nullptr; std::atomic<bool> * cancel = nullptr;
    std::uint64_t base = 0, total = 0;
};
size_t file_write_cb(char * buf, size_t sz, size_t n, void * ud) {
    auto * c = static_cast<DlCb *>(ud);
    return std::fwrite(buf, 1, sz * n, c->fp);
}
int xfer_cb(void * ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto * c = static_cast<DlCb *>(ud);
    if (c->cancel->load(std::memory_order_relaxed)) return 1;
    std::lock_guard<std::mutex> lk(*c->mu);
    c->st->downloaded_bytes = c->base + (std::uint64_t) dlnow;
    if (c->total > 0) c->st->percent = (double) c->st->downloaded_bytes * 100.0 / (double) c->total;
    else if (dltotal > 0) c->st->percent = (double) dlnow * 100.0 / (double) dltotal;
    if (c->st->percent > 100.0) c->st->percent = 100.0;
    return 0;
}
#endif  // EASYAI_HAVE_CURL
}  // namespace

bool ModelsEngine::hf_repo_files(const std::string & repo, std::vector<RepoFile> & out, std::string & err) {
    out.clear();
#if !defined(EASYAI_HAVE_CURL)
    (void) repo; err = "server built without libcurl"; return false;
#else
    if (repo.empty() || repo.find("..") != std::string::npos) { err = "invalid repo"; return false; }
    std::string url = "https://huggingface.co/api/models/" + repo + "/tree/main?recursive=true";
    std::string body;
    if (!curl_get_string(url, body, err)) return false;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_array()) { err = "unexpected HF response"; return false; }
        for (const auto & e : j) {
            if (e.value("type", std::string()) != "file") continue;
            std::string path = e.value("path", std::string());
            if (!ends_with_ci(path, ".gguf")) continue;
            RepoFile f; f.path = path; f.size_bytes = e.value("size", (std::uint64_t) 0);
            out.push_back(std::move(f));
        }
    } catch (const std::exception & e) { err = std::string("parse: ") + e.what(); return false; }
    if (out.empty()) err = "no .gguf files in " + repo;
    return !out.empty();
#endif
}

int ModelsEngine::start_download(const std::string & repo, const std::string & filename, std::string & err) {
#if !defined(EASYAI_HAVE_CURL)
    (void) repo; (void) filename; err = "server built without libcurl"; return -1;
#else
    std::lock_guard<std::mutex> start_lk(dl_start_mu_);
    { std::lock_guard<std::mutex> lk(dl_mu_); if (st_.state == "downloading") { err = "a download is already in progress"; return -1; } }

    std::vector<RepoFile> files;
    if (!hf_repo_files(repo, files, err)) return -1;

    std::vector<RepoFile> target; std::string label;
    if (!filename.empty()) {
        if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos || !ends_with_ci(filename, ".gguf")) {
            err = "invalid filename"; return -1;
        }
        std::string fam = family_base(filename);
        for (auto & f : files) if (family_base(base_name(f.path)) == fam) target.push_back(f);
        if (target.empty()) { RepoFile f; f.path = filename; target.push_back(f); }
        label = filename;
    } else {
        std::string best_fam; int best_rank = 1 << 30; std::uint64_t best_size = 0;
        for (auto & f : files) {
            int rk = quant_rank(base_name(f.path));
            if (rk < best_rank || (rk == best_rank && f.size_bytes > best_size)) { best_rank = rk; best_fam = family_base(base_name(f.path)); best_size = f.size_bytes; }
        }
        for (auto & f : files) if (family_base(base_name(f.path)) == best_fam) target.push_back(f);
        label = best_fam + ".gguf";
    }
    if (target.empty()) { err = "no matching .gguf"; return -1; }
    std::sort(target.begin(), target.end(), [](const RepoFile & a, const RepoFile & b) { return a.path < b.path; });

    for (auto & f : target) {
        fs::path dest; std::string verr;
        if (!safe_in_dir(download_dir_, base_name(f.path), dest, verr)) { err = verr + " (" + base_name(f.path) + ")"; return -1; }
        std::error_code ec;
        if (fs::exists(dest, ec)) { err = "already downloaded: " + base_name(f.path) + " — delete it first"; return -1; }
    }

    if (worker_.joinable()) worker_.join();
    int id;
    { std::lock_guard<std::mutex> lk(dl_mu_); cancel_.store(false); id = ++next_id_;
      st_ = DownloadStatus{}; st_.id = id; st_.repo = repo; st_.filename = label; st_.state = "downloading"; }
    worker_ = std::thread(&ModelsEngine::download_worker, this, id, repo, target);
    easyai::log::write("[models] download #%d %s (%zu file(s)) -> %s\n", id, repo.c_str(), target.size(), download_dir_.c_str());
    return id;
#endif
}

void ModelsEngine::download_worker(int id, std::string repo, std::vector<RepoFile> files) {
#if defined(EASYAI_HAVE_CURL)
    std::uint64_t grand_total = 0; for (auto & f : files) grand_total += f.size_bytes;
    { std::lock_guard<std::mutex> lk(dl_mu_); st_.total_bytes = grand_total; }
    std::uint64_t completed = 0; bool ok = true; std::string err;
    for (auto & f : files) {
        if (cancel_.load()) { ok = false; err = "cancelled"; break; }
        std::string name = base_name(f.path);
        fs::path dest; if (!safe_in_dir(download_dir_, name, dest, err)) { ok = false; break; }
        std::string part = dest.string() + ".part";
        { std::lock_guard<std::mutex> lk(dl_mu_); st_.filename = name; }
        std::FILE * fp = std::fopen(part.c_str(), "wb");
        if (!fp) { ok = false; err = "cannot open " + part; break; }
        CURL * c = curl_easy_init();
        if (!c) { std::fclose(fp); ok = false; err = "curl_easy_init failed"; break; }
        DlCb cb; cb.fp = fp; cb.mu = &dl_mu_; cb.st = &st_; cb.cancel = &cancel_; cb.base = completed; cb.total = grand_total;
        std::string url = "https://huggingface.co/" + repo + "/resolve/main/" + f.path;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &cb);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cb);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        apply_common_curl(c);
        CURLcode rc = curl_easy_perform(c);
        long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c); std::fclose(fp);
        if (rc == CURLE_ABORTED_BY_CALLBACK) { std::remove(part.c_str()); ok = false; err = "cancelled"; break; }
        if (rc != CURLE_OK) { std::remove(part.c_str()); ok = false; err = std::string("curl: ") + curl_easy_strerror(rc); break; }
        if (code >= 400) { std::remove(part.c_str()); ok = false; err = "HTTP " + std::to_string(code); break; }
        std::error_code ec; fs::rename(part, dest, ec);
        if (ec) { std::remove(part.c_str()); ok = false; err = "rename failed: " + ec.message(); break; }
        std::uint64_t sz = f.size_bytes; if (sz == 0) { std::error_code se; sz = (std::uint64_t) fs::file_size(dest, se); if (se) sz = 0; }
        completed += sz; { std::lock_guard<std::mutex> lk(dl_mu_); st_.downloaded_bytes = completed; }
    }
    std::lock_guard<std::mutex> lk(dl_mu_);
    if (ok) { st_.state = "done"; st_.percent = 100.0; st_.downloaded_bytes = grand_total ? grand_total : st_.downloaded_bytes;
        easyai::log::write("[models] download #%d done: %s\n", id, repo.c_str()); }
    else if (err == "cancelled") { st_.state = "idle"; st_.error.clear(); easyai::log::write("[models] download #%d cancelled\n", id); }
    else { st_.state = "error"; st_.error = err; easyai::log::error("[models] download #%d failed: %s\n", id, err.c_str()); }
#else
    (void) id; (void) repo; (void) files;
    std::lock_guard<std::mutex> lk(dl_mu_); st_.state = "error"; st_.error = "server built without libcurl";
#endif
}

void ModelsEngine::cancel_download() { cancel_.store(true, std::memory_order_relaxed); }
ModelsEngine::DownloadStatus ModelsEngine::download_status() {
    std::lock_guard<std::mutex> lk(dl_mu_); return st_;
}

// ===========================================================================
// Live HuggingFace source for the Recommend tab
// ===========================================================================
#if defined(EASYAI_HAVE_CURL)
namespace {

std::string url_encode(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string o; o.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char) c);
        else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 15]); }
    }
    return o;
}

// Pull a params figure (billions) out of a repo name ("…-7B…", "…-0.5B",
// "…-500M"): the largest token wins. nullopt if none.
std::optional<double> params_from_name(const std::string & name) {
    static const std::regex re(R"((\d+(?:\.\d+)?)\s*([bBmM]))");
    double best = -1;
    for (auto it = std::sregex_iterator(name.begin(), name.end(), re); it != std::sregex_iterator(); ++it) {
        double v; try { v = std::stod((*it)[1].str()); } catch (...) { continue; }
        char u = (char) std::tolower((unsigned char) (*it)[2].str()[0]);
        double b = (u == 'm') ? v / 1000.0 : v;
        if (b > best) best = b;
    }
    if (best > 0) return best;
    return std::nullopt;
}

// HuggingFace model search restricted to GGUF repos.
bool hf_search(const std::string & query, int limit, const std::string & sort,
               std::vector<HfHit> & out, std::string & err) {
    std::string url = "https://huggingface.co/api/models?filter=gguf&direction=-1&limit=" +
                      std::to_string(limit) + "&sort=" + (sort.empty() ? "downloads" : sort);
    if (!query.empty()) url += "&search=" + url_encode(query);
    std::string body;
    if (!curl_get_string(url, body, err)) return false;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_array()) { err = "unexpected HF response"; return false; }
        for (const auto & m : j) {
            HfHit h;
            h.repo = m.value("id", std::string());
            if (h.repo.empty()) continue;
            auto sl = h.repo.find('/');
            h.owner     = sl == std::string::npos ? h.repo : h.repo.substr(0, sl);
            h.downloads = m.value("downloads", (std::uint64_t) 0);
            h.likes     = m.value("likes", (std::uint64_t) 0);
            h.pipeline  = m.value("pipeline_tag", std::string());
            out.push_back(std::move(h));
        }
    } catch (const std::exception & e) { err = std::string("parse: ") + e.what(); return false; }
    return true;
}

// Enrich a hit into a scoreable ModelEntry by listing the repo's GGUF files
// (best quant by preference order; params from the name, else from file size).
bool hf_build_entry(const HfHit & h, ModelEntry & out, std::uint64_t & best_size, std::string & err) {
    if (h.repo.empty() || h.repo.find("..") != std::string::npos) { err = "invalid repo"; return false; }
    std::string url = "https://huggingface.co/api/models/" + h.repo + "/tree/main?recursive=true";
    std::string body;
    if (!curl_get_string(url, body, err)) return false;
    std::string best_path; std::uint64_t bsize = 0; int best_rank = 1 << 30;
    try {
        auto j = nlohmann::json::parse(body);
        if (!j.is_array()) { err = "unexpected tree"; return false; }
        for (const auto & e : j) {
            if (e.value("type", std::string()) != "file") continue;
            std::string path = e.value("path", std::string());
            if (!ends_with_ci(path, ".gguf")) continue;
            int rk = quant_rank(base_name(path));
            std::uint64_t sz = e.value("size", (std::uint64_t) 0);
            if (rk < best_rank || (rk == best_rank && sz > bsize)) { best_rank = rk; best_path = path; bsize = sz; }
        }
    } catch (const std::exception & e) { err = std::string("parse: ") + e.what(); return false; }
    if (best_path.empty()) { err = "no .gguf files"; return false; }
    best_size = bsize;

    std::string quant;
    {
        static const char * order[] = { "Q8_0","Q6_K_L","Q6_K","Q5_K_M","Q5_K_S","Q5_0","Q4_K_M","Q4_K_S","Q4_0",
            "Q3_K_L","Q3_K_M","Q3_K_S","Q2_K","IQ4_XS","IQ3_M","IQ2_M","IQ1_M","F16","BF16","F32" };
        std::string uc = to_upper(base_name(best_path));
        for (auto * o : order) if (uc.find(o) != std::string::npos) { quant = o; break; }
    }
    if (quant.empty()) quant = "Q4_K_M";

    double pb;
    auto pn = params_from_name(h.repo);
    if (pn) pb = *pn;
    else    pb = bsize > 0 ? (double) bsize / quant_bpp(quant) / 1e9 : 7.0;

    out = ModelEntry{};
    out.name = h.repo; out.provider = h.owner; out.quantization = quant;
    out.parameters_raw = (std::uint64_t) (pb * 1e9); out.has_params_raw = true;
    char pc[32];
    if (pb >= 1) std::snprintf(pc, sizeof(pc), "%.1fB", pb);
    else         std::snprintf(pc, sizeof(pc), "%dM", (int) std::round(pb * 1000));
    out.parameter_count = pc;
    out.context_length = 0;          // unknown without reading the GGUF header
    out.is_moe = false;
    out.min_ram_gb = pb * quant_bpp(quant) + 0.5;
    out.recommended_ram_gb = out.min_ram_gb * 1.25;
    out.gguf_sources = { { h.repo, h.owner } };
    return true;
}

}  // namespace
#endif  // EASYAI_HAVE_CURL

std::string ModelsEngine::models_json(const std::string & query) {
    auto q = parse_query(query);
    SystemSpecs s = detect_system();
    apply_sim(s, qnum(q, "ram_gb", qnum(q, "ram", -1)), qnum(q, "vram_gb", qnum(q, "memory", -1)),
              (int) qnum(q, "cpu_cores", -1));
    ordered_json env = system_to_json(s);
#if !defined(EASYAI_HAVE_CURL)
    env["total_models"] = 0; env["returned_models"] = 0; env["models"] = ordered_json::array();
    env["error"] = "server built without libcurl";
    return env.dump();
#else
    std::string search  = qget(q, "search");
    std::string runtime = qget(q, "runtime", "any");
    std::string usecase = qget(q, "use_case", "all");
    std::string sort    = qget(q, "sort", "score");
    std::string force   = qget(q, "force_runtime");
    std::string min_fit = qget(q, "min_fit", "marginal");
    bool include_tt = qget(q, "include_too_tight", "true") != "false";
    int limit = (int) qnum(q, "limit", qnum(q, "n", 30));
    if (limit <= 0) limit = 30; if (limit > 80) limit = 80;
    int min_fit_rank = min_fit == "perfect" ? 4 : min_fit == "good" ? 3 : min_fit == "too_tight" ? 1 : 2;

    std::string hf_sort = (sort == "date") ? "lastModified" : (sort == "likes") ? "likes" : "downloads";
    std::vector<HfHit> hits; std::string err;
    if (!hf_search(search, limit, hf_sort, hits, err)) {
        env["total_models"] = 0; env["returned_models"] = 0; env["models"] = ordered_json::array();
        env["error"] = "HuggingFace: " + err;
        return env.dump();
    }

    // Resolve from cache or fetch (bounded-concurrency) the uncached repos.
    std::vector<CachedHf> all; all.reserve(hits.size());
    std::vector<HfHit> to_fetch;
    {
        std::lock_guard<std::mutex> lk(hf_cache_mu_);
        for (auto & h : hits) {
            auto it = hf_cache_->by_repo.find(h.repo);
            if (it != hf_cache_->by_repo.end()) { CachedHf c = it->second; c.downloads = h.downloads; c.likes = h.likes; all.push_back(std::move(c)); }
            else to_fetch.push_back(h);
        }
    }
    const std::size_t MAXC = 8;
    for (std::size_t i = 0; i < to_fetch.size(); i += MAXC) {
        std::vector<std::future<CachedHf>> batch;
        for (std::size_t k = i; k < std::min(i + MAXC, to_fetch.size()); ++k) {
            HfHit h = to_fetch[k];
            batch.push_back(std::async(std::launch::async, [h]() {
                CachedHf c; c.repo = h.repo; c.downloads = h.downloads; c.likes = h.likes;
                std::string e; c.ok = hf_build_entry(h, c.entry, c.best_size, e);
                return c;
            }));
        }
        for (auto & f : batch) {
            CachedHf c = f.get();
            { std::lock_guard<std::mutex> lk(hf_cache_mu_); hf_cache_->by_repo[c.repo] = c; }
            all.push_back(std::move(c));
        }
    }

    // Score. FitRow holds a pointer into all[i].entry — `all` is stable now.
    std::vector<FitRow> rows;
    for (auto & c : all) {
        if (!c.ok) continue;
        if (usecase != "all" && infer_use_case(c.entry) != usecase) continue;
        FitRow r = score_model(c.entry, s, 0, force);
        r.hf_downloads = c.downloads; r.hf_likes = c.likes; r.file_size = c.best_size;
        if (runtime != "any" && r.runtime != runtime) continue;
        if (r.fit_level == "too_tight" && !include_tt) continue;
        if (fit_rank(r.fit_level) < min_fit_rank) continue;
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [&](const FitRow & a, const FitRow & b) {
        if (sort == "tps")       return a.estimated_tps > b.estimated_tps;
        if (sort == "params")    return a.m->params_b() > b.m->params_b();
        if (sort == "mem")       return a.memory_required_gb < b.memory_required_gb;
        if (sort == "downloads") return a.hf_downloads > b.hf_downloads;
        if (sort == "likes")     return a.hf_likes > b.hf_likes;
        return a.score > b.score;
    });

    ordered_json models = ordered_json::array();
    for (const auto & r : rows) models.push_back(fit_to_json(r));
    env["total_models"] = (int) rows.size();
    env["returned_models"] = (int) rows.size();
    env["source"] = "huggingface";
    env["models"] = models;
    return env.dump();
#endif
}

std::string ModelsEngine::plan_json(const std::string & body) {
    ordered_json b;
    try { b = ordered_json::parse(body); } catch (...) { return "{\"error\":\"invalid JSON\"}"; }
    std::string name = b.value("model", "");
    std::uint32_t ctx = b.value("context", 8192u);
    std::string quant = b.value("quant", "");
    std::string kv_quant = b.value("kv_quant", "fp16");
    SystemSpecs s = detect_system();
    apply_sim(s, b.value("ram_gb", -1.0), b.value("vram_gb", -1.0), (int) b.value("cpu_cores", -1));
#if !defined(EASYAI_HAVE_CURL)
    return "{\"error\":\"server built without libcurl\"}";
#else
    HfHit h; h.repo = name;
    auto sl = name.find('/'); h.owner = sl == std::string::npos ? name : name.substr(0, sl);
    ModelEntry m; std::uint64_t sz = 0; std::string err;
    if (!hf_build_entry(h, m, sz, err)) return "{\"error\":\"model '" + name + "': " + err + "\"}";
    if (quant.empty()) quant = m.quantization;

    double model_mem = estimate_memory_gb(m, quant, ctx, kv_quant);
    FitRow cur = score_model(m, s, ctx, "");
    double rec_vram = std::max(m.recommended_ram_gb, model_mem * 1.2);
    double min_ram = std::max(model_mem * 0.2, 8.0);

    ordered_json kv_alts = ordered_json::array();
    double baseline_kv = kv_cache_gb(m, ctx, "fp16");
    for (const std::string & kvq : { std::string("fp16"), std::string("fp8"), std::string("q8_0"), std::string("q4_0") }) {
        double kvgb = kv_cache_gb(m, ctx, kvq);
        double mem = estimate_memory_gb(m, quant, ctx, kvq);
        double sav = baseline_kv > 0 ? std::max(1.0 - kvgb / baseline_kv, 0.0) : 0.0;
        kv_alts.push_back({ {"kv_quant", kvq}, {"memory_required_gb", round2(mem)},
                            {"kv_cache_gb", round2(kvgb)}, {"savings_fraction", round2(sav)}, {"supported", true} });
    }
    ordered_json out = {
        {"model_name", m.name}, {"provider", m.provider},
        {"context", ctx}, {"quantization", quant}, {"kv_quant", kv_quant},
        {"estimate_notice", "Heuristic estimate — for HuggingFace models, params/quant are inferred from the GGUF file size/name."},
        {"minimum",     { {"vram_gb", round2(model_mem)}, {"ram_gb", round2(min_ram)}, {"cpu_cores", 4} }},
        {"recommended", { {"vram_gb", round2(rec_vram)}, {"ram_gb", round2(std::max(min_ram * 1.25, 12.0))},
                          {"cpu_cores", std::max(s.cpu_cores, 8)} }},
        {"current", { {"fit_level", cur.fit_level}, {"run_mode", cur.run_mode},
                      {"estimated_tps", round1(cur.estimated_tps)},
                      {"memory_required_gb", round2(cur.memory_required_gb)},
                      {"memory_available_gb", round2(cur.memory_available_gb)} }},
        {"kv_alternatives", kv_alts},
    };
    return out.dump();
#endif
}

}  // namespace easyai
