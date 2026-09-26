#include "nexus_db/Io.h"

#include "sd_storage/File.h"
#include "sd_storage/Fs.h"

namespace nexus_db {
namespace {

class SdLogFile : public LogFile {
public:
    explicit SdLogFile(sd_storage::File file) : m_file(std::move(file)) {}

    bool readAt(uint32_t offset, void* dst, size_t len) override {
        return m_file.seek(long(offset)) && m_file.readExact(dst, len);
    }
    bool append(const void* src, size_t len) override {
        return m_file.seek(0, SEEK_END) && m_file.writeAll(src, len);
    }
    long size() override { return m_file.size(); }
    bool sync() override { return m_file.sync(); }
    bool truncate(uint32_t length) override { return m_file.truncate(long(length)); }

private:
    sd_storage::File m_file;
};

class SdIo : public Io {
public:
    std::unique_ptr<LogFile> open(const char* path, bool create) override {
        return wrap(sd_storage::File::open(path, create ? sd_storage::Mode::UpdateOrCreate : sd_storage::Mode::Update));
    }
    std::unique_ptr<LogFile> create(const char* path) override {
        // Update mode after truncating: the engine reads back what it wrote.
        sd_storage::File f = sd_storage::File::open(path, sd_storage::Mode::UpdateOrCreate);
        if (!f || !f.truncate(0)) return nullptr;
        return wrap(std::move(f));
    }
    bool exists(const char* path) override { return sd_storage::Fs::exists(path); }
    bool remove(const char* path) override { return sd_storage::Fs::remove(path); }
    bool rename(const char* from, const char* to) override { return sd_storage::Fs::rename(from, to); }
    bool mkdirs(const char* dir) override { return sd_storage::Fs::isDir(dir) || sd_storage::Fs::mkdirs(dir); }

private:
    static std::unique_ptr<LogFile> wrap(sd_storage::File f) {
        if (!f) return nullptr;
        return std::make_unique<SdLogFile>(std::move(f));
    }
};

}  // namespace

Io& sdIo() {
    static SdIo io;
    return io;
}

}  // namespace nexus_db
