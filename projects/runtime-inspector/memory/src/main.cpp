#include "memory_lab/arena.hpp"
#include "memory_lab/experiments.hpp"
#include "memory_lab/layout.hpp"
#include "memory_lab/pmr.hpp"
#include "memory_lab/pool.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void quoted(std::ostream& out, std::string_view text) {
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32) out << "\\u00" << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 15];
        else out << static_cast<char>(c);
    }
    out << '"';
}

std::uint64_t number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("options require unsigned decimal integers");
    return value;
}

void layouts_json(std::ostream& out) {
    const auto layouts = memory_lab::fixture_layouts();
    out << "{\"fixture_layouts\":[";
    for (std::size_t i = 0; i < layouts.size(); ++i) {
        if (i) out << ',';
        const auto& l = layouts[i];
        out << "{\"name\":"; quoted(out, l.name);
        out << ",\"object_size\":" << l.object_size << ",\"object_alignment\":" << l.object_alignment
            << ",\"array_stride\":" << l.array_stride << ",\"internal_padding\":" << l.internal_padding
            << ",\"tail_padding\":" << l.tail_padding << ",\"members\":[";
        for (std::size_t j = 0; j < l.members.size(); ++j) {
            if (j) out << ',';
            const auto& m = l.members[j];
            out << "{\"name\":"; quoted(out, m.name);
            out << ",\"offset\":" << m.offset << ",\"size\":" << m.size << ",\"alignment\":" << m.alignment << '}';
        }
        out << "],\"padding\":[";
        for (std::size_t j = 0; j < l.padding.size(); ++j) {
            if (j) out << ',';
            const auto& p = l.padding[j];
            out << "{\"offset\":" << p.offset << ",\"size\":" << p.size << ",\"tail\":" << (p.tail ? "true" : "false") << '}';
        }
        out << "],\"byte_owner\":[";
        for (std::size_t j = 0; j < l.byte_owner.size(); ++j) { if (j) out << ','; out << l.byte_owner[j]; }
        out << "]}";
    }
    out << "],\"interpretation\":\"active ABI observations, not universal offsets or guaranteed reorder savings; internal gaps align members, tail padding permits aligned array stride. Padding contents are never read; explicit field encoding is needed for portable wire formats. Object alignment, allocation alignment and cache-line spacing are different properties.\"}";
}

void core_json(std::ostream& out) {
    alignas(64) std::array<std::byte, 1024> bytes{};
    memory_lab::BumpArena arena(std::span<std::byte>(bytes).subspan(1));
    void* first = arena.allocate(7, 8);
    void* second = arena.allocate(31, 64);
    if (!first || !second) throw std::logic_error("arena demonstration unexpectedly exhausted");
    const auto consumed = arena.consumed();
    const bool rejected = arena.allocate(std::numeric_limits<std::size_t>::max(), 64) == nullptr
                          && arena.consumed() == consumed;
    if (!rejected || reinterpret_cast<std::uintptr_t>(first) % 8 != 0
        || reinterpret_cast<std::uintptr_t>(second) % 64 != 0)
        throw std::logic_error("arena demonstration invariant failed");
    out << ",\"arena\":{\"capacity\":" << arena.capacity() << ",\"consumed\":" << consumed
        << ",\"payload_bytes\":" << arena.payload_bytes() << ",\"alignment_padding\":" << arena.padding_bytes()
        << ",\"remaining\":" << arena.remaining() << ",\"first_alignment_remainder\":" << reinterpret_cast<std::uintptr_t>(first) % 8
        << ",\"second_alignment_remainder\":" << reinterpret_cast<std::uintptr_t>(second) % 64
        << ",\"huge_failure_preserves_state\":" << (rejected ? "true" : "false");
    arena.reset(); // raw blocks contain no nontrivial live objects
    if (arena.consumed() != 0) throw std::logic_error("arena reset invariant failed");
    out << ",\"consumed_after_reset\":" << arena.consumed()
        << ",\"policy\":\"non-owning, single-threaded; no individual free; reset does not run destructors and invalidates old handles\"}";

    std::array<memory_lab::SlotState, 4> metadata{};
    memory_lab::FixedPool pool(std::span<std::byte>(bytes).subspan(1, 255), metadata, 24, 16);
    void* slot = pool.allocate();
    if (!slot) throw std::logic_error("pool demonstration unexpectedly exhausted");
    const bool released = pool.deallocate(slot);
    void* reused = pool.allocate();
    const bool same = slot == reused;
    if (!pool.deallocate(reused)) throw std::logic_error("pool demonstration valid release failed");
    if (!released || !same || pool.live_count() != 0) throw std::logic_error("pool reuse invariant failed");
    out << ",\"fixed_pool\":{\"backing_capacity\":" << pool.backing_capacity()
        << ",\"payload_size\":" << pool.payload_size() << ",\"alignment\":" << pool.alignment()
        << ",\"slot_stride\":" << pool.slot_stride() << ",\"capacity\":" << pool.capacity()
        << ",\"live_count\":" << pool.live_count() << ",\"free_count\":" << pool.free_count()
        << ",\"alignment_padding\":" << pool.alignment_padding() << ",\"tail_bytes\":" << pool.tail_bytes()
        << ",\"metadata_reserved_bytes\":" << pool.metadata_reserved_bytes()
        << ",\"metadata_used_bytes\":" << pool.metadata_used_bytes()
        << ",\"rounding_per_live_slot\":" << pool.slot_stride() - pool.payload_size()
        << ",\"release_reuse_ok\":" << (released && same ? "true" : "false")
        << ",\"policy\":\"bounded fixed-size individual reclamation, not general malloc; invalid-release checks O(capacity); caller owns object lifetimes\"}";
}

void environment_json(std::ostream& out) {
    out << "{\"compiler\":";
    quoted(out, __VERSION__);
#if defined(__APPLE__)
    out << ",\"os\":\"macOS\"";
#elif defined(__linux__)
    out << ",\"os\":\"Linux\"";
#else
    out << ",\"os\":\"other\"";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    out << ",\"isa\":\"arm64\"";
#elif defined(__x86_64__)
    out << ",\"isa\":\"x86_64\"";
#else
    out << ",\"isa\":\"other\"";
#endif
#if defined(_LIBCPP_VERSION)
    out << ",\"standard_library\":\"libc++\",\"standard_library_version\":" << _LIBCPP_VERSION;
#elif defined(__GLIBCXX__)
    out << ",\"standard_library\":\"libstdc++\",\"standard_library_version\":" << __GLIBCXX__;
#endif
    out << ",\"cplusplus\":" << __cplusplus << ",\"pointer_bits\":" << sizeof(void*) * 8;
#if defined(__OPTIMIZE__)
    out << ",\"optimized\":true";
#else
    out << ",\"optimized\":false";
#endif
    out << ",\"flags\":null,\"source_identity\":null,\"identity_reason\":\"attach exact compiler command and source hashes in evidence/report wrapper; binary cannot infer them\"}";
}
} // namespace

int main(int argc, char** argv) {
    try {
        memory_lab::ExperimentConfig config;
        bool seen_size = false, seen_seed = false, seen_repetitions = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (option == "--help" && argc == 2) {
                std::cout << "runtime-memory [--size 1..2048] [--seed UINT64] [--repetitions 1..20]\n"
                             "Defaults: size=128 seed=42 repetitions=3. Emits memory-lab-v1 JSON.\n";
                return 0;
            }
            if (++i == argc) throw std::invalid_argument("option missing value");
            const auto value = number(argv[i]);
            if (option == "--seed" && !seen_seed) { config.seed = value; seen_seed = true; }
            else if (option == "--size" && !seen_size && value <= 2048) {
                config.size = static_cast<std::size_t>(value); seen_size = true;
            } else if (option == "--repetitions" && !seen_repetitions && value <= 20) {
                config.repetitions = static_cast<std::size_t>(value); seen_repetitions = true;
            } else throw std::invalid_argument("unknown, duplicate or out-of-range option");
        }
        memory_lab::validate_config(config);
        const auto pmr = memory_lab::observe_pmr(config.size, config.seed);
        const auto experiments = memory_lab::run_experiments(config);
        bool ok = pmr.monotonic_fallback.fixture_ok && pmr.monotonic_hardcap.fixture_ok && pmr.pool.fixture_ok
                  && pmr.monotonic_hardcap.bad_alloc_observed && pmr.copy_move_values_ok
                  && pmr.copy_uses_default && pmr.explicit_copy_uses_destination && pmr.move_uses_source
                  && pmr.explicit_move_uses_destination && pmr.move_assignment_keeps_destination
                  && !pmr.monotonic_fallback.bad_alloc_observed && pmr.monotonic_fallback.upstream.allocations > 0
                  && pmr.monotonic_fallback.upstream_before_block_free == pmr.monotonic_fallback.upstream_after_block_free;
        for (const auto* route : {&pmr.monotonic_fallback, &pmr.monotonic_hardcap, &pmr.pool})
            ok = ok && route->constructed == route->destroyed && route->constructed > 0
                 && route->frontend.current_bytes == 0 && route->final_upstream_bytes == 0;
        for (const auto& backend : experiments.backends)
            for (const auto& sample : backend.samples) ok = ok && sample.raw.fixture_ok && sample.objects.fixture_ok;
        std::ostringstream out;
        out << std::setprecision(17) << "{\"schema\":\"memory-lab-v1\",\"fixture_ok\":" << (ok ? "true" : "false")
            << ",\"config\":{\"size\":" << config.size << ",\"seed\":" << config.seed << ",\"repetitions\":" << config.repetitions
            << "},\"environment\":";
        environment_json(out);
        out << ",\"layout\":"; layouts_json(out);
        core_json(out);
        out << ",\"pmr\":"; memory_lab::write_pmr_json(out, pmr);
        out << ",\"experiments\":"; memory_lab::write_experiments_json(out, experiments);
        out << "}\n";
        std::cout << out.str();
        return ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "runtime-memory: " << error.what() << '\n';
        return 2;
    }
}
