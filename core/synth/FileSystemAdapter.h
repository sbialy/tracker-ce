#pragma once
#ifdef FEATURE_SYNTH
#include "Platform.h"
#include "PolySynth/FileSystem.h"

class FileSystemAdapter : public polysynth::FileSystem
{
  public:
    FileSystemAdapter(const std::string& pathPrefix, ::FileSystem* platformFileSystem);

    std::vector<std::string_view> listFiles(const std::string& path,
                                            std::function<bool(const ::std::string& name)> matcher,
                                            tiny_optional<int> maxFiles) override;

    std::optional<polysynth::FileSystem::FileHandle> open(
      const std::string& path,
      polysynth::FileSystem::OpenOptions options) override;
    void close(FileHandle file) override;
    bool write(FileHandle file, std::span<const uint8_t> data) override;
    int32_t read(FileHandle file, std::span<uint8_t> buf) override;
    bool seek(FileHandle file, int offset) override;

    void createFolder(const std::string& path) override;
    bool replace(const std::string& from, const std::string& to) override;
    bool remove(const std::string& path) override;

    // `to` becomes the file `from`, both full paths. `to` keeps its old content unless the replacement is complete: it is moved
    // aside first and moved back when the rename of `from` fails. False: `to` is as it was.
    static bool replaceFile(::FileSystem* fileSystem, const char* from, const char* to);

    static void staticInit();

  private:
    ::FileSystem* platformFileSystem;
    std::string pathPrefix;

    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    static char* fileNamesBuffer;
    static char** listDataName;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    int fileCount;
};
#endif
