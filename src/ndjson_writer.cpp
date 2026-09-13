#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "stuttometer/ndjson_writer.hpp"
#include <system_error>
#include <cstring>
#include <string>

namespace stuttometer {

NdjsonWriter::NdjsonWriter(std::FILE* file, bool owns_file, std::filesystem::path base_path, size_t max_bytes, size_t max_files)
    : file_(file)
    , owns_file_(owns_file)
    , base_path_(std::move(base_path))
    , max_bytes_(max_bytes)
    , max_files_(max_files)
{
    ring_ = std::make_unique<Cell[]>(RING_CAPACITY);
    for (size_t i = 0; i < RING_CAPACITY; ++i) {
        ring_[i].sequence.store(i, std::memory_order_relaxed);
    }
    worker_thread_ = std::thread(&NdjsonWriter::worker_loop, this);
}

std::unique_ptr<NdjsonWriter> NdjsonWriter::create_for_file(
    const std::filesystem::path& base_path,
    size_t max_bytes_per_file,
    size_t max_files
) {
    if (base_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(base_path.parent_path(), ec);
    }
#if defined(_WIN32)
    std::FILE* f = _wfopen(base_path.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(base_path.string().c_str(), "wb");
#endif
    if (!f) return nullptr;
    return std::unique_ptr<NdjsonWriter>(new NdjsonWriter(f, true, base_path, max_bytes_per_file, max_files));
}

std::unique_ptr<NdjsonWriter> NdjsonWriter::create_for_stream(std::FILE* stream) {
    if (!stream) return nullptr;
    return std::unique_ptr<NdjsonWriter>(new NdjsonWriter(stream, false, {}, 0, 0));
}

NdjsonWriter::~NdjsonWriter() {
    stop();
}

void NdjsonWriter::stop() noexcept {
    bool expected_stopped = false;
    if (stopped_.compare_exchange_strong(expected_stopped, true, std::memory_order_release)) {
        running_.store(false, std::memory_order_release);
        resume_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (file_) {
            std::fflush(file_);
            if (owns_file_) {
                std::fclose(file_);
            }
            file_ = nullptr;
        }
    }
}

void NdjsonWriter::pause_worker_for_test() noexcept {
    test_paused_.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lock(pause_mutex_);
    pause_cv_.wait(lock, [this]() { return worker_is_paused_.load(std::memory_order_acquire); });
}

void NdjsonWriter::resume_worker_for_test() noexcept {
    test_paused_.store(false, std::memory_order_release);
    resume_cv_.notify_all();
}

bool NdjsonWriter::push(const EtwEventRecord& record) noexcept {
    if (stopped_.load(std::memory_order_acquire)) return false;

    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = ring_[pos & RING_MASK];
        size_t seq = cell.sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (dif == 0) {
            if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.record = record;
                cell.sequence.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
            return false;
        } else {
            pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }
}

void NdjsonWriter::rotate_files() {
    if (!owns_file_ || !file_) return;
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;

    std::error_code ec;
    if (max_files_ == 1) {
        // Truncate active file
#if defined(_WIN32)
        file_ = _wfopen(base_path_.c_str(), L"wb");
#else
        file_ = std::fopen(base_path_.string().c_str(), "wb");
#endif
    } else if (max_files_ > 1) {
        // Delete oldest rotated file if it exists: base.(max_files - 1)
        std::filesystem::path oldest = base_path_.string() + "." + std::to_string(max_files_ - 1);
        std::filesystem::remove(oldest, ec);

        // Rename base.i -> base.(i+1) for i = max_files - 2 down to 1
        for (size_t i = max_files_ - 2; i >= 1; --i) {
            std::filesystem::path src = base_path_.string() + "." + std::to_string(i);
            std::filesystem::path dst = base_path_.string() + "." + std::to_string(i + 1);
            if (std::filesystem::exists(src, ec)) {
                std::filesystem::rename(src, dst, ec);
            }
        }

        // Rename base -> base.1
        if (std::filesystem::exists(base_path_, ec)) {
            std::filesystem::path dst1 = base_path_.string() + ".1";
            std::filesystem::rename(base_path_, dst1, ec);
        }

        // Reopen base_path_
#if defined(_WIN32)
        file_ = _wfopen(base_path_.c_str(), L"wb");
#else
        file_ = std::fopen(base_path_.string().c_str(), "wb");
#endif
    }
    current_file_bytes_ = 0;
}

void NdjsonWriter::worker_loop() {
    // Worker busy-polls the ring; do not replace with a blocking wait, or pause_worker_for_test() will not observe the pause request.
    while (running_.load(std::memory_order_relaxed) || enqueue_pos_.load(std::memory_order_relaxed) != dequeue_pos_.load(std::memory_order_relaxed)) {
        if (test_paused_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(pause_mutex_);
                worker_is_paused_.store(true, std::memory_order_release);
            }
            pause_cv_.notify_all();

            std::unique_lock<std::mutex> lock(pause_mutex_);
            resume_cv_.wait(lock, [this]() {
                return !test_paused_.load(std::memory_order_acquire) || !running_.load(std::memory_order_relaxed);
            });
            worker_is_paused_.store(false, std::memory_order_release);
        }

        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell& cell = ring_[pos & RING_MASK];
        size_t seq = cell.sequence.load(std::memory_order_acquire);
        intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

        if (dif == 0) {
            if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                EtwEventRecord rec = cell.record;
                cell.sequence.store(pos + RING_MASK + 1, std::memory_order_release);

                const std::string_view cat_sv = category_to_string(static_cast<EventCategory>(rec.category));
                char buf[256];
                int len = std::snprintf(buf, sizeof(buf),
                    "{\"v\":%u,\"ts_qpc\":%llu,\"cat\":\"%.*s\",\"id\":%u,\"pid\":%u,\"tid\":%u,\"cpu\":%u,\"dur_us\":%u,\"aux\":%llu,\"flags\":%u}\n",
                    NDJSON_LINE_SCHEMA_VERSION,
                    static_cast<unsigned long long>(rec.qpc_timestamp),
                    static_cast<int>(cat_sv.size()), cat_sv.data(),
                    static_cast<unsigned int>(rec.event_id),
                    static_cast<unsigned int>(rec.pid),
                    static_cast<unsigned int>(rec.tid),
                    static_cast<unsigned int>(rec.cpu_index),
                    static_cast<unsigned int>(rec.duration_us),
                    static_cast<unsigned long long>(rec.auxiliary_data),
                    static_cast<unsigned int>(rec.flags)
                );

                if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
                    if (owns_file_ && max_bytes_ > 0 && current_file_bytes_ + static_cast<size_t>(len) > max_bytes_) {
                        rotate_files();
                    }
                    if (file_) {
                        std::fwrite(buf, 1, static_cast<size_t>(len), file_);
                        current_file_bytes_ += static_cast<size_t>(len);
                        written_records_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        } else if (dif < 0) {
            if (!running_.load(std::memory_order_relaxed) && enqueue_pos_.load(std::memory_order_relaxed) == dequeue_pos_.load(std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::yield();
        }
    }
}

} // namespace stuttometer
