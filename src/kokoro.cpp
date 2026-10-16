#include "easyai/kokoro.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <glob.h>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unordered_map>

#include "onnxruntime_c_api.h"

namespace easyai {

namespace {

#include "kokoro_vocab.inc"

constexpr int    kOrtApiVersionNeeded = 16;      // oldest API table with everything used here (ORT >= 1.16)
constexpr int    kStyleDim            = 256;
constexpr int    kStyleRows           = 510;
constexpr std::size_t kMaxPhonemes    = 510;     // the model's context
constexpr float  kSentencePauseS      = 0.25f;
constexpr float  kClausePauseS        = 0.10f;
constexpr float  kTrimTopDb           = 60.0f;
constexpr int    kTrimFrame           = 2048;
constexpr int    kTrimHop             = 512;
constexpr float  kMinSpeed            = 0.5f, kMaxSpeed = 2.0f;
constexpr int    kEspeakCharsUtf8     = 1;
constexpr int    kEspeakPhonemesIpa   = 0x02;
constexpr int    kEspeakOutputSynchronous = 2;
constexpr int    kEspeakDontExit      = 0x8000;
constexpr const char * kSentenceMarks = ".!?\xE2\x80\xA6";          // . ! ? …
constexpr const char * kClauseMarks   = ",;:";
const char * kPunctuation[] = {";", ":", ",", ".", "!", "?", "\xC2\xA1", "\xC2\xBF", "\xE2\x80\x94", "\xE2\x80\xA6",
                               "\"", "\xC2\xAB", "\xC2\xBB", "\xE2\x80\x9C", "\xE2\x80\x9D", "(", ")", "{", "}", "[", "]"};

bool file_exists(const std::string & p) { struct stat st; return !p.empty() && ::stat(p.c_str(), &st) == 0; }

std::vector<std::string> glob_paths(const std::string & pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (::glob(pattern.c_str(), 0, nullptr, &g) == 0)
        for (std::size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    ::globfree(&g);
    std::sort(out.rbegin(), out.rend());   // newest version first
    return out;
}

std::string first_existing(const std::vector<std::string> & patterns) {
    for (const auto & p : patterns) for (const auto & m : glob_paths(p)) if (file_exists(m)) return m;
    return "";
}

std::string read_file(const std::string & p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::size_t utf8_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4; }

// ---- espeak-ng through dlopen ----------------------------------------------
struct Espeak {
    void * lib = nullptr;
    int    (*init)(int, int, const char *, int) = nullptr;
    int    (*set_voice)(const char *) = nullptr;
    const char * (*to_phonemes)(const void **, int, int) = nullptr;
    int    (*terminate)() = nullptr;
    std::string path, voice;
    std::mutex  mu;   // espeak keeps global state
};

// ---- onnxruntime through dlopen ----------------------------------------------
struct Ort {
    void *           lib = nullptr;
    const OrtApi *   api = nullptr;
    OrtEnv *         env = nullptr;
    OrtSessionOptions * opts = nullptr;
    OrtSession *     session = nullptr;
    OrtMemoryInfo *  mem = nullptr;
    std::string      path;
    std::string      tokens_name = "input_ids";
    ONNXTensorElementDataType speed_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    std::string      output_name;
};

}  // namespace

// ---------------------------------------------------------------------------
// model-free helpers
// ---------------------------------------------------------------------------
namespace kokoro {

std::string lang_of_voice(const std::string & voice) {
    switch (voice.empty() ? 'a' : voice[0]) {
        case 'p': return "pt-br";
        case 'b': return "en-gb";
        case 'e': return "es";
        case 'f': return "fr-fr";
        case 'i': return "it";
        case 'h': return "hi";
        case 'j': return "ja";
        case 'z': return "cmn";
        default:  return "en-us";
    }
}

std::string clean_phonemes(const std::string & s) {
    std::string t;
    t.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') {                       // language switch flag "(en)"
            const auto k = s.find(')', i);
            if (k != std::string::npos && k - i <= 6) { i = k; continue; }
        }
        if (s[i] == '_') continue;               // espeak phoneme separator
        t.push_back(s[i] == '\n' || s[i] == '\t' ? ' ' : s[i]);
    }
    std::string out; bool space = true;
    for (char c : t) {
        if (c == ' ') { if (!space) out.push_back(' '); space = true; }
        else { out.push_back(c); space = false; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

float pause_after(const std::string & batch, float sentence_s, float clause_s) {
    std::size_t e = batch.size();
    while (e > 0 && batch[e - 1] == ' ') --e;
    if (e == 0) return 0;
    // last UTF-8 code point
    std::size_t b = e - 1;
    while (b > 0 && ((unsigned char) batch[b] & 0xC0) == 0x80) --b;
    const std::string last = batch.substr(b, e - b);
    if (std::string(kSentenceMarks).find(last) != std::string::npos && (last.size() > 1 || std::strchr(".!?", last[0]))) return sentence_s;
    if (last.size() == 1 && std::strchr(kClauseMarks, last[0])) return clause_s;
    return 0;
}

namespace {
// Cut s into pieces at level: 0 after sentence marks, 1 after clause marks, 2 at spaces.
std::vector<std::string> atoms_at(const std::string & s, int level) {
    std::vector<std::string> out; std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        cur.push_back(s[i]);
        if (s[i] != ' ') continue;
        const std::string before = cur.substr(0, cur.size() - 1);
        bool cut = level == 2;
        if (!cut && !before.empty()) {
            const float p = pause_after(before, 1, level == 0 ? 0 : 1);
            cut = p > 0;
        }
        if (cut) { out.push_back(before); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    for (auto & a : out) while (!a.empty() && a.back() == ' ') a.pop_back();
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string & a) { return a.empty(); }), out.end());
    return out;
}
void atoms(const std::string & s, std::size_t max_len, int level, std::vector<std::string> & out) {
    if (s.size() <= max_len) { if (!s.empty()) out.push_back(s); return; }
    if (level > 2) {                                   // mid-word, last resort
        for (std::size_t i = 0; i < s.size(); i += max_len) out.push_back(s.substr(i, max_len));
        return;
    }
    for (const auto & a : atoms_at(s, level)) atoms(a, max_len, level + 1, out);
}
// Greedy packing of atoms into batches of at most limit chars (a space between atoms).
std::vector<std::pair<std::size_t, std::size_t>> pack(const std::vector<std::size_t> & len, std::size_t limit) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    std::size_t start = 0, cur = 0;
    for (std::size_t i = 0; i < len.size(); ++i) {
        const std::size_t add = cur == 0 ? len[i] : cur + 1 + len[i];
        if (cur != 0 && add > limit) { out.push_back({start, i}); start = i; cur = len[i]; }
        else cur = add;
    }
    if (start < len.size()) out.push_back({start, len.size()});
    return out;
}
}  // namespace

std::vector<std::string> split_batches(const std::string & phonemes, std::size_t max_len) {
    std::vector<std::string> a;
    std::string s = clean_phonemes(phonemes);
    atoms(s, max_len, 0, a);
    if (a.empty()) return {};
    std::vector<std::size_t> len;
    for (const auto & x : a) len.push_back(x.size());
    // Balance: the smallest limit that needs no extra batch, so no short tail batch.
    const std::size_t fewest = pack(len, max_len).size();
    std::size_t lo = *std::max_element(len.begin(), len.end()), hi = max_len;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (pack(len, mid).size() <= fewest) hi = mid; else lo = mid + 1;
    }
    std::vector<std::string> out;
    for (const auto & r : pack(len, lo)) {
        std::string b;
        for (std::size_t i = r.first; i < r.second; ++i) { if (!b.empty()) b += ' '; b += a[i]; }
        out.push_back(b);
    }
    return out;
}

bool load_npz_float32(const std::string & z, std::vector<std::pair<std::string, std::vector<float>>> & out, std::string & err) {
    // Sizes come from the central directory: numpy writes the local headers in
    // streaming mode (sizes 0, data descriptor after the data).
    out.clear();
    auto rd16 = [&](std::size_t p) { return (std::uint32_t) (unsigned char) z[p] | ((std::uint32_t) (unsigned char) z[p + 1] << 8); };
    auto rd32 = [&](std::size_t p) { return rd16(p) | (rd16(p + 2) << 16); };
    auto rd64 = [&](std::size_t p) { return (std::uint64_t) rd32(p) | ((std::uint64_t) rd32(p + 4) << 32); };
    constexpr std::uint32_t kEocd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50, kZip64Locator = 0x07064b50, kZip64Eocd = 0x06064b50;
    constexpr std::uint32_t kMax32 = 0xFFFFFFFFu;
    if (z.size() < 22) { err = "npz: too small"; return false; }
    std::size_t eocd = z.size() - 22;
    while (eocd > 0 && rd32(eocd) != kEocd) --eocd;
    if (rd32(eocd) != kEocd) { err = "npz: no end-of-central-directory"; return false; }
    std::uint64_t n_entries = rd16(eocd + 10), cd_off = rd32(eocd + 16);
    if (cd_off == kMax32 && eocd >= 20 && rd32(eocd - 20) == kZip64Locator) {   // zip64
        const std::uint64_t z64 = rd64(eocd - 20 + 8);
        if (z64 + 56 > z.size() || rd32((std::size_t) z64) != kZip64Eocd) { err = "npz: bad zip64 record"; return false; }
        n_entries = rd64((std::size_t) z64 + 32); cd_off = rd64((std::size_t) z64 + 48);
    }
    std::size_t c = (std::size_t) cd_off;
    for (std::uint64_t e = 0; e < n_entries; ++e) {
        if (c + 46 > z.size() || rd32(c) != kCentral) { err = "npz: bad central directory"; return false; }
        const std::uint32_t method = rd16(c + 10), nlen = rd16(c + 28), xlen = rd16(c + 30), clen = rd16(c + 32);
        std::uint64_t csize = rd32(c + 20), usize = rd32(c + 24), loff = rd32(c + 42);
        const std::string name = z.substr(c + 46, nlen);
        // zip64 extra (id 0x0001): the 64-bit values replace the ones that read 0xFFFFFFFF, in this order.
        for (std::size_t x = c + 46 + nlen, end = x + xlen; x + 4 <= end;) {
            const std::uint32_t id = rd16(x), len = rd16(x + 2);
            std::size_t f = x + 4;
            if (id == 0x0001) {
                if (usize == kMax32 && f + 8 <= end) { usize = rd64(f); f += 8; }
                if (csize == kMax32 && f + 8 <= end) { csize = rd64(f); f += 8; }
                if (loff  == kMax32 && f + 8 <= end) { loff  = rd64(f); f += 8; }
            }
            x += 4 + len;
        }
        c += 46 + nlen + xlen + clen;
        if (method != 0) { err = "npz: entry " + name + " is compressed (only stored npz is supported)"; return false; }
        const std::size_t lh = (std::size_t) loff;
        if (lh + 30 > z.size() || rd32(lh) != kLocal) { err = "npz: bad local header for " + name; return false; }
        const std::size_t data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
        if (data + csize > z.size()) { err = "npz: truncated entry " + name; return false; }
        if (csize < 10 || z.compare(data, 6, "\x93NUMPY") != 0) { err = "npz: " + name + " is not .npy"; return false; }
        const unsigned major = (unsigned char) z[data + 6];
        const std::size_t hlen = major == 1 ? rd16(data + 8) : rd32(data + 8);
        const std::size_t hstart = data + (major == 1 ? 10 : 12);
        const std::string header = z.substr(hstart, hlen);
        if (header.find("'<f4'") == std::string::npos) { err = "npz: " + name + " is not float32 little-endian"; return false; }
        const std::size_t body = hstart + hlen, nbytes = (std::size_t) (data + csize - body);
        std::vector<float> v(nbytes / 4);
        std::memcpy(v.data(), z.data() + body, v.size() * 4);
        std::string key = name;
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".npy") == 0) key.resize(key.size() - 4);
        out.emplace_back(key, std::move(v));
    }
    if (out.empty()) { err = "npz: no entries"; return false; }
    return true;
}

}  // namespace kokoro

// ---------------------------------------------------------------------------
// Kokoro
// ---------------------------------------------------------------------------

struct Kokoro::Impl {
    Config  cfg;
    bool    loaded = false;
    Ort     ort;
    Espeak  es;
    std::unordered_map<std::string, int>                 vocab;
    std::vector<std::pair<std::string, std::vector<float>>> voices;

    ~Impl() {
        if (ort.api) {
            if (ort.session) ort.api->ReleaseSession(ort.session);
            if (ort.opts)    ort.api->ReleaseSessionOptions(ort.opts);
            if (ort.mem)     ort.api->ReleaseMemoryInfo(ort.mem);
            if (ort.env)     ort.api->ReleaseEnv(ort.env);
        }
        if (es.lib && es.terminate) es.terminate();
        if (es.lib)  ::dlclose(es.lib);
        if (ort.lib) ::dlclose(ort.lib);
    }

    bool ort_ok(OrtStatus * st, const char * what, std::string & err) {
        if (!st) return true;
        err = std::string(what) + ": " + ort.api->GetErrorMessage(st);
        ort.api->ReleaseStatus(st);
        return false;
    }

    bool load_ort(std::string & err) {
        std::string path = cfg.onnxruntime;
        if (path.empty()) { const char * e = std::getenv("EASYAI_ONNXRUNTIME"); if (e && *e) path = e; }
        if (path.empty()) path = first_existing({
            "/opt/easyai/onnxruntime/lib/libonnxruntime.so*", "/opt/easyai/onnxruntime/lib/libonnxruntime.dylib",
            "/usr/local/lib/libonnxruntime.so*", "/usr/lib/libonnxruntime.so*",
            "/opt/homebrew/lib/libonnxruntime.dylib", "/opt/Homebrew/lib/libonnxruntime.dylib", "/usr/local/lib/libonnxruntime.dylib"});
        if (path.empty()) { err = "onnxruntime: shared library not found (set [TTS] onnxruntime = /path/libonnxruntime.so)"; return false; }
        ort.lib = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!ort.lib) { err = std::string("onnxruntime: dlopen ") + path + ": " + ::dlerror(); return false; }
        using base_fn = const OrtApiBase * (*)();
        auto get_base = (base_fn) ::dlsym(ort.lib, "OrtGetApiBase");
        if (!get_base) { err = "onnxruntime: OrtGetApiBase missing in " + path; return false; }
        ort.api = get_base()->GetApi(kOrtApiVersionNeeded);
        if (!ort.api) { err = "onnxruntime: " + path + " is older than API " + std::to_string(kOrtApiVersionNeeded); return false; }
        ort.path = path;
        if (!ort_ok(ort.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "easyai-kokoro", &ort.env), "CreateEnv", err)) return false;
        if (!ort_ok(ort.api->CreateSessionOptions(&ort.opts), "CreateSessionOptions", err)) return false;
        if (cfg.threads > 0) ort.api->SetIntraOpNumThreads(ort.opts, cfg.threads);
        ort.api->SetSessionGraphOptimizationLevel(ort.opts, ORT_ENABLE_ALL);
        if (!ort_ok(ort.api->CreateSession(ort.env, cfg.model.c_str(), ort.opts, &ort.session), "CreateSession", err)) return false;
        if (!ort_ok(ort.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &ort.mem), "CreateCpuMemoryInfo", err)) return false;

        // Input names / types: "input_ids" (newer) or "tokens" (older), speed float or int.
        OrtAllocator * alloc = nullptr;
        if (!ort_ok(ort.api->GetAllocatorWithDefaultOptions(&alloc), "GetAllocator", err)) return false;
        std::size_t n_in = 0;
        ort.api->SessionGetInputCount(ort.session, &n_in);
        for (std::size_t i = 0; i < n_in; ++i) {
            char * name = nullptr;
            if (!ort_ok(ort.api->SessionGetInputName(ort.session, i, alloc, &name), "SessionGetInputName", err)) return false;
            const std::string n = name;
            alloc->Free(alloc, name);
            if (n == "tokens" || n == "input_ids") ort.tokens_name = n;
            if (n == "speed") {
                OrtTypeInfo * ti = nullptr;
                if (ort_ok(ort.api->SessionGetInputTypeInfo(ort.session, i, &ti), "SessionGetInputTypeInfo", err)) {
                    const OrtTensorTypeAndShapeInfo * tsi = nullptr;
                    if (ort.api->CastTypeInfoToTensorInfo(ti, &tsi) == nullptr && tsi) ort.api->GetTensorElementType(tsi, &ort.speed_type);
                    ort.api->ReleaseTypeInfo(ti);
                }
            }
        }
        char * out_name = nullptr;
        if (!ort_ok(ort.api->SessionGetOutputName(ort.session, 0, alloc, &out_name), "SessionGetOutputName", err)) return false;
        ort.output_name = out_name;
        alloc->Free(alloc, out_name);

        // Vocabulary: the model's own (metadata "kokoro_config") wins over the built-in table.
        for (const auto & kv : kKokoroVocab) vocab[kv.sym] = kv.id;
        OrtModelMetadata * meta = nullptr;
        if (ort.api->SessionGetModelMetadata(ort.session, &meta) == nullptr && meta) {
            char * cfgjson = nullptr;
            if (ort.api->ModelMetadataLookupCustomMetadataMap(meta, alloc, "kokoro_config", &cfgjson) == nullptr && cfgjson) {
                parse_vocab_json(cfgjson);
                alloc->Free(alloc, cfgjson);
            }
            ort.api->ReleaseModelMetadata(meta);
        }
        return true;
    }

    // {"vocab": {"sym": id, ...}} — tiny purpose-built scan, no JSON library needed here.
    void parse_vocab_json(const std::string & j) {
        const auto v = j.find("\"vocab\"");
        if (v == std::string::npos) return;
        std::unordered_map<std::string, int> parsed;
        std::size_t i = j.find('{', v);
        if (i == std::string::npos) return;
        ++i;
        while (i < j.size() && j[i] != '}') {
            const auto q1 = j.find('"', i); if (q1 == std::string::npos) break;
            std::string key; std::size_t k = q1 + 1;
            while (k < j.size() && j[k] != '"') {
                if (j[k] == '\\' && k + 1 < j.size()) {
                    ++k;
                    if (j[k] == 'u' && k + 4 < j.size()) {          // \uXXXX → UTF-8 (BMP only, enough for the vocab)
                        const unsigned cp = (unsigned) std::stoul(j.substr(k + 1, 4), nullptr, 16);
                        if (cp < 0x80) key.push_back((char) cp);
                        else if (cp < 0x800) { key.push_back((char) (0xC0 | (cp >> 6))); key.push_back((char) (0x80 | (cp & 0x3F))); }
                        else { key.push_back((char) (0xE0 | (cp >> 12))); key.push_back((char) (0x80 | ((cp >> 6) & 0x3F))); key.push_back((char) (0x80 | (cp & 0x3F))); }
                        k += 5; continue;
                    }
                    key.push_back(j[k]); ++k; continue;
                }
                key.push_back(j[k]); ++k;
            }
            const auto colon = j.find(':', k);
            if (colon == std::string::npos) break;
            const int id = std::atoi(j.c_str() + colon + 1);
            parsed[key] = id;
            i = j.find_first_of(",}", colon);
            if (i == std::string::npos) break;
            if (j[i] == ',') ++i;
        }
        if (!parsed.empty()) vocab = std::move(parsed);
    }

    bool load_espeak(std::string & err) {
        std::string lib = cfg.espeak_lib, data = cfg.espeak_data;
        if (lib.empty()) { const char * e = std::getenv("PHONEMIZER_ESPEAK_LIBRARY"); if (e && *e) lib = e; }
        if (data.empty()) { const char * e = std::getenv("ESPEAK_DATA_PATH"); if (e && *e) data = std::string(e) + "/espeak-ng-data"; }
        if (lib.empty()) lib = first_existing({
            "/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1", "/usr/lib/aarch64-linux-gnu/libespeak-ng.so.1", "/usr/lib/arm-linux-gnueabihf/libespeak-ng.so.1",
            "/usr/lib/libespeak-ng.so.1", "/usr/local/lib/libespeak-ng.so.1",
            "/opt/homebrew/lib/libespeak-ng.dylib", "/opt/Homebrew/lib/libespeak-ng.dylib",
            "/opt/homebrew/Cellar/espeak-ng/*/lib/libespeak-ng.dylib", "/opt/Homebrew/Cellar/espeak-ng/*/lib/libespeak-ng.dylib",
            "/usr/local/lib/libespeak-ng.dylib"});
        if (data.empty()) data = first_existing({
            "/usr/lib/x86_64-linux-gnu/espeak-ng-data", "/usr/lib/aarch64-linux-gnu/espeak-ng-data", "/usr/share/espeak-ng-data",
            "/opt/homebrew/share/espeak-ng-data", "/opt/Homebrew/share/espeak-ng-data",
            "/opt/homebrew/Cellar/espeak-ng/*/share/espeak-ng-data", "/opt/Homebrew/Cellar/espeak-ng/*/share/espeak-ng-data",
            "/usr/local/share/espeak-ng-data"});
        if (lib.empty())  { err = "espeak-ng: shared library not found (apt install espeak-ng / brew install espeak-ng, or [TTS] espeak_lib)"; return false; }
        if (data.empty()) { err = "espeak-ng: espeak-ng-data directory not found ([TTS] espeak_data)"; return false; }
        es.lib = ::dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!es.lib) { err = std::string("espeak-ng: dlopen ") + lib + ": " + ::dlerror(); return false; }
        es.init        = (int (*)(int, int, const char *, int)) ::dlsym(es.lib, "espeak_Initialize");
        es.set_voice   = (int (*)(const char *)) ::dlsym(es.lib, "espeak_SetVoiceByName");
        es.to_phonemes = (const char * (*)(const void **, int, int)) ::dlsym(es.lib, "espeak_TextToPhonemes");
        es.terminate   = (int (*)()) ::dlsym(es.lib, "espeak_Terminate");
        if (!es.init || !es.set_voice || !es.to_phonemes) { err = "espeak-ng: symbols missing in " + lib; return false; }
        // espeak_Initialize wants the directory that CONTAINS espeak-ng-data.
        const auto slash = data.find_last_of('/');
        const std::string parent = slash == std::string::npos ? "." : data.substr(0, slash);
        if (es.init(kEspeakOutputSynchronous, 0, parent.c_str(), kEspeakDontExit) < 0) {
            err = "espeak-ng: espeak_Initialize failed with data at " + data; return false;
        }
        es.path = lib;
        return true;
    }

    bool load_voices(std::string & err) {
        const std::string bytes = read_file(cfg.voices);
        if (bytes.empty()) { err = "kokoro: cannot read voices file " + cfg.voices; return false; }
        if (!kokoro::load_npz_float32(bytes, voices, err)) return false;
        for (const auto & v : voices)
            if (v.second.size() != (std::size_t) kStyleRows * kStyleDim) { err = "kokoro: voice " + v.first + " has an unexpected shape"; return false; }
        return true;
    }

    const std::vector<float> * voice(const std::string & name) const {
        for (const auto & v : voices) if (v.first == name) return &v.second;
        return nullptr;
    }

    // text → IPA phonemes, punctuation kept and attached as phonemizer does.
    bool phonemize(const std::string & text, const std::string & lang, std::string & out, std::string & err) {
        std::lock_guard<std::mutex> lk(es.mu);
        if (es.voice != lang) {
            if (es.set_voice(lang.c_str()) != 0) { err = "espeak-ng: no voice '" + lang + "'"; return false; }
            es.voice = lang;
        }
        out.clear();
        std::string chunk;
        auto flush_chunk = [&]() {
            std::string t = chunk; chunk.clear();
            const auto a = t.find_first_not_of(" \t\n"), b = t.find_last_not_of(" \t\n");
            if (a == std::string::npos) return;
            t = t.substr(a, b - a + 1);
            const void * ptr = t.c_str();
            std::string ph;
            while (ptr) {
                const char * r = es.to_phonemes(&ptr, kEspeakCharsUtf8, kEspeakPhonemesIpa);
                if (r && *r) { if (!ph.empty()) ph += ' '; ph += r; }
            }
            ph = kokoro::clean_phonemes(ph);
            if (ph.empty()) return;
            if (!out.empty() && out.back() != ' ') out += ' ';
            out += ph;
        };
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t L = std::min(utf8_len((unsigned char) text[i]), text.size() - i);
            const std::string cp = text.substr(i, L);
            bool mark = false;
            for (const char * m : kPunctuation) if (cp == m) { mark = true; break; }
            if (mark) { flush_chunk(); out += cp; }
            else chunk += cp;
            i += L;
        }
        flush_chunk();
        // keep only what the model knows
        std::string kept;
        for (std::size_t i = 0; i < out.size();) {
            const std::size_t L = std::min(utf8_len((unsigned char) out[i]), out.size() - i);
            const std::string cp = out.substr(i, L);
            if (vocab.count(cp)) kept += cp;
            i += L;
        }
        out = kokoro::clean_phonemes(kept);
        return true;
    }

    std::vector<std::int64_t> tokenize(const std::string & ph) const {
        std::vector<std::int64_t> t;
        for (std::size_t i = 0; i < ph.size();) {
            const std::size_t L = std::min(utf8_len((unsigned char) ph[i]), ph.size() - i);
            const auto it = vocab.find(ph.substr(i, L));
            if (it != vocab.end()) t.push_back(it->second);
            i += L;
        }
        return t;
    }

    bool infer(const std::vector<std::int64_t> & tokens, const float * style, float speed, std::vector<float> & audio, std::string & err) {
        std::vector<std::int64_t> ids; ids.reserve(tokens.size() + 2);
        ids.push_back(0); ids.insert(ids.end(), tokens.begin(), tokens.end()); ids.push_back(0);
        const std::int64_t ids_shape[2] = {1, (std::int64_t) ids.size()};
        const std::int64_t style_shape[2] = {1, kStyleDim};
        const std::int64_t speed_shape[1] = {1};
        float speed_f = speed; std::int32_t speed_i = (std::int32_t) std::lround(speed); std::int64_t speed_l = speed_i;
        OrtValue * in[3] = {nullptr, nullptr, nullptr};
        const char * names[3] = {ort.tokens_name.c_str(), "style", "speed"};
        bool ok = ort_ok(ort.api->CreateTensorWithDataAsOrtValue(ort.mem, ids.data(), ids.size() * sizeof(std::int64_t), ids_shape, 2,
                                                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), "tokens tensor", err)
                && ort_ok(ort.api->CreateTensorWithDataAsOrtValue(ort.mem, (void *) style, kStyleDim * sizeof(float), style_shape, 2,
                                                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), "style tensor", err);
        if (ok) {
            if (ort.speed_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
                ok = ort_ok(ort.api->CreateTensorWithDataAsOrtValue(ort.mem, &speed_i, sizeof speed_i, speed_shape, 1, ort.speed_type, &in[2]), "speed tensor", err);
            else if (ort.speed_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
                ok = ort_ok(ort.api->CreateTensorWithDataAsOrtValue(ort.mem, &speed_l, sizeof speed_l, speed_shape, 1, ort.speed_type, &in[2]), "speed tensor", err);
            else
                ok = ort_ok(ort.api->CreateTensorWithDataAsOrtValue(ort.mem, &speed_f, sizeof speed_f, speed_shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), "speed tensor", err);
        }
        OrtValue * out = nullptr;
        const char * out_names[1] = {ort.output_name.c_str()};
        if (ok) ok = ort_ok(ort.api->Run(ort.session, nullptr, names, in, 3, out_names, 1, &out), "Run", err);
        if (ok) {
            float * data = nullptr;
            OrtTensorTypeAndShapeInfo * info = nullptr;
            std::size_t n = 0;
            ok = ort_ok(ort.api->GetTensorMutableData(out, (void **) &data), "GetTensorMutableData", err)
              && ort_ok(ort.api->GetTensorTypeAndShape(out, &info), "GetTensorTypeAndShape", err)
              && ort_ok(ort.api->GetTensorShapeElementCount(info, &n), "GetTensorShapeElementCount", err);
            if (ok) audio.assign(data, data + n);
            if (info) ort.api->ReleaseTensorTypeAndShapeInfo(info);
        }
        for (auto * v : in) if (v) ort.api->ReleaseValue(v);
        if (out) ort.api->ReleaseValue(out);
        return ok;
    }
};

namespace {
// librosa-style trim: frames of RMS more than top_db below the loudest frame are silence at both ends.
void trim_silence(std::vector<float> & a) {
    if (a.size() < (std::size_t) kTrimFrame) return;
    std::vector<float> rms;
    for (std::size_t off = 0; off + kTrimFrame <= a.size(); off += kTrimHop) {
        double acc = 0;
        for (int i = 0; i < kTrimFrame; ++i) acc += (double) a[off + i] * a[off + i];
        rms.push_back((float) std::sqrt(acc / kTrimFrame));
    }
    const float peak = *std::max_element(rms.begin(), rms.end());
    if (peak <= 0) return;
    const float thr = peak * std::pow(10.0f, -kTrimTopDb / 20.0f);
    std::size_t first = 0, last = rms.size();
    while (first < rms.size() && rms[first] <= thr) ++first;
    while (last > first && rms[last - 1] <= thr) --last;
    if (first >= last) return;
    const std::size_t s = first * kTrimHop, e = std::min(a.size(), last * kTrimHop + (std::size_t) kTrimFrame);
    a = std::vector<float>(a.begin() + (std::ptrdiff_t) s, a.begin() + (std::ptrdiff_t) e);
}
}  // namespace

Kokoro::Kokoro() : p_(new Impl) {}
Kokoro::~Kokoro() = default;
bool Kokoro::loaded() const { return p_->loaded; }
const std::string & Kokoro::onnxruntime_path() const { return p_->ort.path; }
const std::string & Kokoro::espeak_path() const { return p_->es.path; }
bool Kokoro::has_voice(const std::string & name) const { return p_->voice(name) != nullptr; }
std::vector<std::string> Kokoro::voice_names() const {
    std::vector<std::string> v; for (const auto & x : p_->voices) v.push_back(x.first); return v;
}

bool Kokoro::load(const Config & cfg, std::string & err) {
    p_.reset(new Impl);
    p_->cfg = cfg;
    if (!file_exists(cfg.model))  { err = "kokoro: model not found: " + cfg.model; return false; }
    if (!file_exists(cfg.voices)) { err = "kokoro: voices not found: " + cfg.voices; return false; }
    if (!p_->load_espeak(err)) return false;
    if (!p_->load_ort(err)) return false;
    if (!p_->load_voices(err)) return false;
    p_->loaded = true;
    return true;
}

bool Kokoro::phonemize(const std::string & text, const std::string & lang, std::string & phonemes, std::string & err) {
    if (!p_->es.lib) { err = "kokoro: not loaded"; return false; }
    return p_->phonemize(text, lang, phonemes, err);
}

bool Kokoro::synthesize(const std::string & text, const std::string & voice, float speed,
                        std::vector<std::int16_t> & pcm, std::string & err) {
    pcm.clear();
    if (!p_->loaded) { err = "kokoro: not loaded"; return false; }
    const std::vector<float> * style = p_->voice(voice);
    if (!style) { err = "kokoro: unknown voice '" + voice + "'"; return false; }
    speed = std::min(kMaxSpeed, std::max(kMinSpeed, speed));
    std::string ph;
    if (!p_->phonemize(text, kokoro::lang_of_voice(voice), ph, err)) return false;
    const auto batches = kokoro::split_batches(ph, kMaxPhonemes);
    if (batches.empty()) { err = "kokoro: nothing to say in '" + text + "'"; return false; }
    std::vector<float> all;
    for (std::size_t b = 0; b < batches.size(); ++b) {
        const auto tokens = p_->tokenize(batches[b]);
        if (tokens.empty()) continue;
        const std::size_t row = std::min(tokens.size(), (std::size_t) kStyleRows) - 1;
        std::vector<float> audio;
        if (!p_->infer(tokens, style->data() + row * kStyleDim, speed, audio, err)) return false;
        trim_silence(audio);
        all.insert(all.end(), audio.begin(), audio.end());
        if (b + 1 < batches.size()) {
            const float pause = kokoro::pause_after(batches[b], kSentencePauseS, kClausePauseS);
            all.insert(all.end(), (std::size_t) (pause * kSampleRate), 0.0f);
        }
    }
    if (all.empty()) { err = "kokoro: no phonemes of '" + text + "' are in the vocabulary"; return false; }
    pcm.resize(all.size());
    for (std::size_t i = 0; i < all.size(); ++i)
        pcm[i] = (std::int16_t) std::lround(std::max(-1.0f, std::min(1.0f, all[i])) * 32767.0f);
    return true;
}

}  // namespace easyai
