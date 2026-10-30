#include "gta_spirv_cross.h"
#include "spirv_msl.hpp"
#include "spirv_reflect.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

namespace {
char *copy_text(const char *text) noexcept {
    if (!text) text = "Unknown SPIRV-Cross error";
    const size_t length = std::strlen(text);
    char *copy = static_cast<char *>(std::malloc(length + 1));
    if (copy) std::memcpy(copy, text, length + 1);
    return copy;
}

void validate(const uint32_t *words, size_t nwords) {
    if (!words) throw std::invalid_argument("SPIR-V words is null");
    if (nwords < 5) throw std::invalid_argument("SPIR-V header is truncated");
    if (nwords > 4u * 1024u * 1024u)
        throw std::invalid_argument("SPIR-V exceeds the 16 MiB input limit");
    if (words[0] != 0x07230203u) throw std::invalid_argument("Invalid SPIR-V magic");
    if (!words[3] || words[3] > 1024u * 1024u)
        throw std::invalid_argument("SPIR-V ID bound is zero or exceeds the limit");
    if (words[4]) throw std::invalid_argument("SPIR-V reserved schema is nonzero");
    for (size_t offset = 5; offset < nwords;) {
        const size_t count = words[offset] >> 16;
        if (!count || count > nwords - offset)
            throw std::invalid_argument("SPIR-V instruction has an invalid word count");
        offset += count;
    }
}

std::string translate(const uint32_t *words, size_t nwords, int mode) {
    validate(words, nwords);
    if (mode == 1) {
        spirv_cross::CompilerReflection compiler(words, nwords);
        compiler.set_format("json");
        return compiler.compile();
    }
    if (mode != 0) throw std::invalid_argument("Unknown SPIRV-Cross mode");
    spirv_cross::CompilerMSL compiler(words, nwords);
    auto options = compiler.get_msl_options();
    // Keep the CLI's platform even on iOS: Service.cs Xc has no --msl-ios.
    // Changing the platform here would silently change the v4 translation chain.
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.msl_version = 20300;
    options.argument_buffers = true;
    options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier1;
    options.texture_buffer_native = true;
    options.pad_fragment_output_components = true;
    // main.cpp sets this unconditionally; its API default is false.
    options.ios_support_base_vertex_instance = true;
    compiler.set_msl_options(options);
    return compiler.compile();
}
} // namespace

extern "C" GTA_SPVC_EXPORT int gta_spirv_cross(const uint32_t *words,
                                               size_t nwords, int mode,
                                               char **out, char **error) {
    if (out) *out = nullptr;
    if (error) *error = nullptr;
    if (!out || !error || out == error) return -1;
    try {
        const std::string result = translate(words, nwords, mode);
        *out = copy_text(result.c_str());
        if (!*out) throw std::bad_alloc();
        return 0;
    } catch (const std::exception &exception) {
        *error = copy_text(exception.what());
    } catch (...) {
        *error = copy_text("Unknown C++ exception in SPIRV-Cross");
    }
    return -1;
}

extern "C" GTA_SPVC_EXPORT void gta_spirv_cross_free(void *memory) {
    std::free(memory);
}
