// Kokoro-82M in C++ (spec.md "easyai-assistant" rule 10a). Model-free:
// npz reader on a synthetic archive, batching, pauses, phoneme cleaning.
// With EASYAI_KOKORO_MODEL + EASYAI_KOKORO_VOICES (and onnxruntime /
// espeak-ng reachable) also loads the real model, prints the phonemes for a
// pt-BR and an en sentence, and synthesizes both.
#include "easyai/kokoro.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } else { std::printf("ok   %s\n", #cond); } } while (0)

static void put16(std::string & s, unsigned v) { s.push_back((char) (v & 0xFF)); s.push_back((char) ((v >> 8) & 0xFF)); }
static void put32(std::string & s, unsigned v) { put16(s, v & 0xFFFF); put16(s, (v >> 16) & 0xFFFF); }

// A stored .npz the way numpy writes it: local headers with sizes 0xFFFFFFFF
// and a zip64 extra, real sizes in the central directory.
static std::string make_npz(const std::vector<std::pair<std::string, std::vector<float>>> & arrays) {
    std::string z, cd;
    for (const auto & a : arrays) {
        std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" + std::to_string(a.second.size()) + ",), }";
        while ((10 + header.size() + 1) % 64) header.push_back(' ');
        header.push_back('\n');
        std::string npy = "\x93NUMPY\x01"; npy.push_back('\0'); put16(npy, (unsigned) header.size()); npy += header;
        npy.append((const char *) a.second.data(), a.second.size() * 4);
        const std::string name = a.first + ".npy";
        const unsigned off = (unsigned) z.size();
        // local header: sizes unknown, zip64 extra with zeros (numpy's streaming shape)
        z += "PK\x03\x04"; put16(z, 45); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0); put32(z, 0);
        put32(z, 0xFFFFFFFF); put32(z, 0xFFFFFFFF); put16(z, (unsigned) name.size()); put16(z, 20);
        z += name; put16(z, 1); put16(z, 16); put32(z, 0); put32(z, 0); put32(z, 0); put32(z, 0);
        z += npy;
        cd += "PK\x01\x02"; put16(cd, 45); put16(cd, 45); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0);
        put32(cd, (unsigned) npy.size()); put32(cd, (unsigned) npy.size()); put16(cd, (unsigned) name.size()); put16(cd, 0); put16(cd, 0);
        put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off); cd += name;
    }
    const unsigned cd_off = (unsigned) z.size();
    z += cd;
    z += "PK\x05\x06"; put16(z, 0); put16(z, 0); put16(z, (unsigned) arrays.size()); put16(z, (unsigned) arrays.size());
    put32(z, (unsigned) cd.size()); put32(z, cd_off); put16(z, 0);
    return z;
}

int main() {
    using namespace easyai::kokoro;
    // --- npz ---------------------------------------------------------------
    {
        std::vector<std::pair<std::string, std::vector<float>>> in = {{"pm_alex", {1.5f, -2.0f, 3.25f}}, {"am_michael", {0.5f}}}, out;
        std::string err;
        CHECK(load_npz_float32(make_npz(in), out, err));
        CHECK(out.size() == 2 && out[0].first == "pm_alex" && out[0].second.size() == 3 && out[0].second[2] == 3.25f);
        CHECK(out[1].first == "am_michael" && out[1].second[0] == 0.5f);
        CHECK(!load_npz_float32("not a zip", out, err) && !err.empty());
    }
    // --- voices → languages, cleaning ----------------------------------------
    CHECK(lang_of_voice("pm_alex") == "pt-br" && lang_of_voice("am_michael") == "en-us" && lang_of_voice("bm_george") == "en-gb");
    CHECK(clean_phonemes("  ola  (en)wɜːld(pt) m_u_n_d_ʊ \n") == "ola wɜːld mundʊ");
    // --- pauses and batching --------------------------------------------------
    CHECK(pause_after("ola mundʊ.", 0.25f, 0.1f) == 0.25f);
    CHECK(pause_after("ola mundʊ,", 0.25f, 0.1f) == 0.1f);
    CHECK(pause_after("ola mundʊ", 0.25f, 0.1f) == 0.0f);
    CHECK(pause_after("fim\xE2\x80\xA6", 0.25f, 0.1f) == 0.25f);   // …
    {
        // atoms: sentence first, then clause; batches are balanced (no short tail), as kokoro-onnx does
        auto b = split_batches("one two three. four five six, seven eight nine! ten", 20);
        CHECK(b.size() == 4);
        CHECK(b.size() == 4 && b[0] == "one two three." && b[1] == "four five six," && b[2] == "seven eight nine!" && b[3] == "ten");
        auto one = split_batches("short text.", 510);
        CHECK(one.size() == 1 && one[0] == "short text.");
        auto words = split_batches("abcdefghij klmnopqrst uvwxyz", 12);     // no marks: word boundaries
        CHECK(words.size() == 3 && words[0] == "abcdefghij");
        auto mid = split_batches("abcdefghijklmnopqrstuvwxyz", 10);         // one long word: cut mid-word
        CHECK(mid.size() == 3 && mid[0].size() == 10);
        CHECK(split_batches("   ", 10).empty());
    }
    // --- real model (optional) ------------------------------------------------
    const char * model = std::getenv("EASYAI_KOKORO_MODEL"), * voices = std::getenv("EASYAI_KOKORO_VOICES");
    if (model && voices) {
        easyai::Kokoro k; std::string err;
        easyai::Kokoro::Config c; c.model = model; c.voices = voices;
        if (!k.load(c, err)) { std::printf("skip real kokoro: %s\n", err.c_str()); }
        else {
            std::printf("onnxruntime: %s\nespeak-ng: %s\nvoices: %zu\n", k.onnxruntime_path().c_str(), k.espeak_path().c_str(), k.voice_names().size());
            CHECK(k.has_voice("pm_alex") && k.has_voice("am_michael"));
            std::string ph;
            CHECK(k.phonemize("Olá mundo, tudo bem? Vou consultar a ferramenta.", "pt-br", ph, err));
            std::printf("pt phonemes: %s\n", ph.c_str());
            // byte-identical to python phonemizer 3.4 + espeak-ng 1.52; the nasal is u + combining tilde (U+0303), a Kokoro vocab symbol
            CHECK(ph == "olˈa mˈu\xCC\x83" "ŋdʊ, tˈudʊ bˈeɪŋ? vow kˌoŋsuwtˈaɾ a fˌexæmˈeɪŋtæ.");
            CHECK(k.phonemize("Hello world, what time is it?", "en-us", ph, err));
            std::printf("en phonemes: %s\n", ph.c_str());
            CHECK(ph == "həlˈoʊ wˈɜːld, wˌʌt tˈaɪm ɪz ɪt?");
            std::vector<std::int16_t> pcm;
            CHECK(k.synthesize("Olá, tudo bem?", "pm_alex", 1.0f, pcm, err));
            std::printf("pt audio: %.2f s\n", pcm.size() / 24000.0);
            CHECK(pcm.size() > 24000 / 2);
            CHECK(!k.synthesize("Hello", "no_such_voice", 1.0f, pcm, err) && err.find("unknown voice") != std::string::npos);
        }
    } else {
        std::printf("skip real kokoro: set EASYAI_KOKORO_MODEL and EASYAI_KOKORO_VOICES\n");
    }
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
