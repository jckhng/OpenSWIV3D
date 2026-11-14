#pragma once

#include "clock.hpp"
#include "movie_preparation.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swiv3d::detail::smacker_loading {

struct DosTransfer {
    std::uint32_t bytes;
    std::uint64_t before_work, after_work;
};

struct ProtectedRead {
    std::vector<DosTransfer> transfers;
    std::uint64_t bytes = 0;
};

struct OpeningReads {
    std::vector<std::uint32_t> requests;
    std::uint64_t bridge_work = 0, rad_work = 0, cached_work = 0;
    unsigned direct_reads = 0;
    std::vector<ProtectedRead> calls;
    std::size_t table_begin = 0, table_end = 0, direct_begin = 0;
    std::uint64_t first_copy = 0, final_copy = 0;
};

inline std::uint64_t repeated(std::uint64_t count) { return count == 0 ? 1 : count + (count + 3) / 4; }

// Streaming SmackOpen with the original DOS/4GW file-transfer buffer.
inline OpeningReads opening_reads(const std::vector<std::uint8_t>& bytes) {
    constexpr unsigned header_bytes = 104, file_block_bytes = 4096;
    constexpr unsigned dos_transfer_bytes = 8192, rad_transfer_bytes = 61440;
    constexpr unsigned dos_read_limit = 65535, file_open_charge = 1024;
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) { value |= std::uint32_t(bytes.at(offset + byte)) << (byte * 8); }
        return value;
    };
    const auto frames = std::uint64_t(word(12)) + (word(20) & 1);
    const auto tables = word(52);
    const auto data_offset = header_bytes + frames * 5 + tables;
    if (frames == 0 || data_offset > bytes.size()) { throw std::runtime_error("Truncated movie frame index"); }
    const auto first_frame = word(header_bytes) & ~3U;
    if (first_frame > bytes.size() - data_offset) { throw std::runtime_error("Truncated first movie frame"); }
    OpeningReads plan;
    plan.requests.push_back(file_open_charge);
    auto& requests = plan.requests;
    std::uint64_t position = 0;
    const auto read = [&](std::uint64_t count) {
        const auto start = position;
        unsigned protected_calls = 0;
        while (count != 0) {
            const auto block = count > dos_read_limit ? rad_transfer_bytes : count;
            ++protected_calls;
            bool first_transfer = true;
            auto remaining = std::min<std::uint64_t>(block, bytes.size() - position);
            const auto actual = remaining;
            ProtectedRead call;
            call.bytes = actual;
            while (remaining != 0) {
                const auto transfer = std::min<std::uint64_t>(remaining, dos_transfer_bytes);
                requests.push_back(static_cast<std::uint32_t>(transfer));
                // DOS/4GW copies returned bytes from its 8-KiB real-mode buffer.
                // Subsequent transfers reuse the protected-call register frame.
                const auto bridge = (first_transfer ? 1407 : 1090) + repeated(transfer / 2) + transfer % 2;
                // The bridge selects its bounce extent before the DOS callback,
                // then copies returned words after the disk wait completes.
                const std::uint64_t before = (first_transfer ? 902 : 585) + (remaining > dos_transfer_bytes ? 87 : 0);
                const auto after = bridge - before;
                plan.bridge_work += bridge;
                call.transfers.push_back({static_cast<std::uint32_t>(transfer),before,after});
                first_transfer = false;
                remaining -= transfer;
            }
            plan.calls.push_back(std::move(call));
            position += actual;
            if (actual < block) { break; }
            count -= actual;
        }
        if (protected_calls != 0) { plan.rad_work += 36 + (protected_calls - 1) * 19; }
        return position - start;
    };
    read(header_bytes);
    read(frames * 4);
    read(frames);
    plan.table_begin = plan.calls.size();
    read(tables);
    plan.table_end = plan.calls.size();
    std::uint64_t cached = 0;
    if (position % file_block_bytes != 0) { cached = read(file_block_bytes - position % file_block_bytes); }
    const auto first_copy = std::min<std::uint64_t>(first_frame, cached);
    plan.first_copy = first_copy;
    if (first_copy != 0) { plan.cached_work += repeated(first_copy / 4) + repeated(first_copy % 4); }
    auto remaining = first_frame - first_copy;
    constexpr unsigned direct_read_bytes = 4 * file_block_bytes;
    plan.direct_begin = plan.calls.size();
    while (remaining > file_block_bytes) {
        const auto block = std::min<std::uint64_t>(remaining & ~(file_block_bytes - 1U), direct_read_bytes);
        read(block);
        ++plan.direct_reads;
        remaining -= block;
    }
    if (remaining != 0) {
        plan.final_copy = remaining;
        read(file_block_bytes);
        plan.cached_work += repeated(remaining / 4) + repeated(remaining % 4);
    }
    return plan;
}

inline std::vector<std::uint32_t> disk_requests(const std::vector<std::uint8_t>& bytes) {
    return opening_reads(bytes).requests;
}

struct CallerParts {
    std::uint64_t index = 0, palette = 0;
};

inline CallerParts caller_parts(const std::vector<std::uint8_t>& bytes, const OpeningReads& reads) {
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte) { value |= std::uint32_t(bytes.at(offset + byte)) << (byte * 8); }
        return value;
    };
    const auto frames = std::uint64_t(word(12)) + (word(20) & 1);
    const auto encoded = static_cast<std::int32_t>(word(16));
    const auto duration = encoded < 0 ? -std::int64_t(encoded) : std::int64_t(encoded) * 100;
    if ((word(72) & 0xf0000000u) != 0xd0000000u || duration == 0) { return {}; }
    const auto window = std::max<std::uint64_t>(1,100000 / duration);
    const auto data = 104 + frames * 5 + word(52);
    if (frames <= window || data % 4096 == 0 || reads.direct_reads == 0 ||
        reads.first_copy == 0 || reads.final_copy == 0 || reads.table_begin != 3 ||
        reads.direct_begin != reads.table_end + 1) { return {}; }
    std::uint64_t rolling = 0, best = 0;
    for (std::size_t index = 0; index < window; ++index) { rolling += word(104 + index * 4); }
    std::uint64_t work = window * 7;
    std::uint32_t maximum = 0;
    for (std::size_t index = 0; index < frames - window; ++index) {
        work += 2;
        if (rolling > best) {
            work += 2;
            if (index != 0) { best = rolling; work += 2; }
        }
        const auto size = word(104 + index * 4);
        rolling = rolling - size + word(104 + (index + window) * 4);
        work += 15;
        if (size > maximum) { maximum = size; ++work; }
        if (index + 1 < frames - window) { ++work; }
    }
    for (std::size_t index = frames - window; index < frames; ++index) {
        work += 10;
        const auto size = word(104 + index * 4);
        if (size > maximum) { maximum = size; ++work; }
    }
    const auto type = bytes.at(static_cast<std::size_t>(104 + frames * 4));
    if ((type & 1) == 0 || (type & 0xfe) != 2) { return {}; }
    auto offset = data + 1;
    unsigned colour = 0;
    std::uint64_t palette = 113 + repeated(192) + repeated(0);
    while (colour < 256) {
        const auto code = bytes.at(static_cast<std::size_t>(offset));
        if ((code & 0xc0) == 0) { ++colour; offset += 3; palette += 14; continue; }
        const auto count = (code & ((code & 0x80) != 0 ? 0x7f : 0x3f)) + 1;
        colour += count;
        offset += (code & 0x80) != 0 ? 1 : 2;
        palette += ((code & 0x80) != 0 ? 32 : 33) + repeated(count * 3 / 4) + repeated(count * 3 % 4);
    }
    if (colour != 256) { throw std::runtime_error("Invalid opening palette extent"); }
    return {work,palette};
}

// Caller work for the streamed, single-track stereo DPCM opening profile.
// The size-table getter contributes two instructions outside the table builder.
inline std::uint64_t caller_work(const std::vector<std::uint8_t>& bytes, const OpeningReads& reads) {
    const auto parts = caller_parts(bytes,reads);
    if (parts.index == 0) { return 0; }
    constexpr unsigned main_path = 795, library_path = 20345, sound_clears = 48;
    constexpr unsigned timer_reads = 2472, block_read_path = 379;
    return parts.index + main_path + library_path + sound_clears + timer_reads + repeated(299) + repeated(0) +
        parts.palette + block_read_path + reads.direct_reads * 39 + reads.cached_work + reads.rad_work;
}

inline std::uint64_t disk_wait_work(const std::vector<std::uint32_t>& requests) {
    std::uint64_t work = 0;
    for (auto bytes : requests) {
        work += (std::uint64_t(bytes) * CompatibilityClock::cpu_frequency +
                 CompatibilityClock::disk_bytes_per_second - 1) / CompatibilityClock::disk_bytes_per_second;
    }
    return work;
}

inline MoviePreparation preparation(const std::vector<std::uint8_t>& bytes, const OpeningReads& reads,
                                    std::uint64_t tables, std::uint64_t first_frame) {
    MoviePreparation result;
    const auto parts = caller_parts(bytes,reads);
    if (parts.index == 0) {
        result.cpu(tables + first_frame + reads.bridge_work + caller_work(bytes,reads));
        result.disk(disk_wait_work(reads.requests));
        return result;
    }
    const auto read = [&](std::size_t index) {
        for (const auto& transfer : reads.calls.at(index).transfers) {
            result.cpu(transfer.before_work);
            result.disk(disk_wait_work({transfer.bytes}));
            result.cpu(transfer.after_work);
        }
    };
    // The original caller runs these paths between protected DOS services.
    constexpr unsigned open_entry = 239, allocated_header = 694;
    constexpr unsigned header_return = 572, index_return = 506, types_return = 599;
    constexpr unsigned tables_return = 263, index_setup = 204, cache_return = 345;
    constexpr unsigned direct_return = 76, sound_and_return = 21158, sound_clears = 48;
    result.cpu(open_entry);
    result.disk(disk_wait_work({1024}));
    result.cpu(allocated_header);
    read(0);
    result.cpu(header_return);
    read(1);
    result.cpu(index_return);
    read(2);
    result.cpu(types_return);
    std::uint64_t unread_tables = 0;
    for (auto index = reads.table_begin; index < reads.table_end; ++index) { unread_tables += reads.calls[index].bytes; }
    for (auto index = reads.table_begin; index < reads.table_end; ++index) {
        read(index);
        unread_tables -= reads.calls[index].bytes;
        if (unread_tables != 0) { result.cpu(18 + (unread_tables > 65535)); }
    }
    result.cpu(tables_return);
    result.cpu(tables);
    result.cpu(parts.index + index_setup);
    read(reads.table_end);
    result.cpu(cache_return + (reads.calls.at(reads.direct_begin).bytes < 16384) +
               repeated(reads.first_copy / 4) + repeated(reads.first_copy % 4));
    for (unsigned index = 0; index < reads.direct_reads; ++index) {
        read(reads.direct_begin + index);
        result.cpu(direct_return + (index + 1 == reads.direct_reads));
    }
    read(reads.calls.size() - 1);
    result.cpu(sound_and_return + parts.palette + sound_clears +
               repeated(reads.final_copy / 4) + repeated(reads.final_copy % 4));
    result.cpu(first_frame);
    return result;
}

}
