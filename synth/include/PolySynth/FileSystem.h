#pragma once

#include "PolyUtil/misc.h"

#include <functional>
#include <optional>
#include <span>
#include <string>

namespace polysynth
{
/**
 * Abstraction of file system access.
 */
class FileSystem
{
  public:
    using FileHandle = void*;

    enum class OpenOptions
    {
        kNone = 0,
        kWrite = 1,
    };

    /**
     * returns a view to the list of files. It is owned internally so if there's a need for storage
     * or processing, then copy is needed.
     * Subsequent calls to this function invalidate previous results
     */
    virtual std::vector<std::string_view> listFiles(
      const std::string& path,
      std::function<bool(const ::std::string& name)> matcher,
      tiny_optional<int> maxFiles) = 0;

    virtual std::optional<FileHandle> open(const std::string& path, OpenOptions options) = 0;
    virtual void close(FileHandle file) = 0;
    virtual bool write(FileHandle file, std::span<const uint8_t> data) = 0;
    virtual int32_t read(FileHandle file, std::span<uint8_t> buf) = 0;
    virtual bool seek(FileHandle file, int offset) = 0;

    virtual void createFolder(const std::string& path) = 0;

    /**
     * Replaces the file `to` by the file `from` (same folder). The file `to` is never lost: if this returns false it is as it was
     * (`from` may be left behind). The default does nothing and fails.
     */
    virtual bool replace(const std::string& from, const std::string& to)
    {
        (void)from;
        (void)to;
        return false;
    }
    /** Deletes a file. False when it is not there or cannot be deleted. The default does nothing and fails. */
    virtual bool remove(const std::string& path)
    {
        (void)path;
        return false;
    }

    virtual ~FileSystem() = default;
};
}
