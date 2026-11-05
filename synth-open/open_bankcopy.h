// Open PolySynth shim for MK66 - copy of the patch bank files of a part between two folders (project save and load).
// Plain C++ over a small file operations type, so the host smoke test runs the same code with an in-memory card that fails on request.
#pragma once

#include "open_load.h"

#include <cstdint>
#include <string>

namespace polysynth::open
{

enum class CopyResult
{
    kCopied,
    kAbsent, // the source file is not there
    kFailed, // the source could not be read or the destination written: the destination file is as it was
};

// The files of a part: its slot files 0..15, the slot 0 backup and the slot names, in this order. Name without folder.
constexpr int kBankFiles = kBackupSlot + 2;
inline std::string bankFileName(int part, int index)
{
    char name[8];
    patchSlotName(part, index <= kBackupSlot ? index : 0, name);
    return std::string(name) + (index <= kBackupSlot ? ".patch" : kSlotNamesExt);
}

// Copies the bank files of `part` from `srcDir` to `dstDir`. `Ops` provides
//   CopyResult copy(const std::string& src, const std::string& dst)  kAbsent when src does not exist, else the whole file or kFailed
//   bool exists(const std::string& path)
//   bool remove(const std::string& path)
//   bool sourceReadable()   the source folder can be read, so a file that is not there is really not there
// Every file that exists in the source replaces the destination file only when it was copied completely. A file the source does not have
// is removed from the destination (a slot never survives from another project), but only after every copy worked and only when the
// source folder is readable. Slot 0 is never removed (stock firmware knows only that file). False: some file could not be copied or
// removed, the caller reports it. The files that were not touched are as they were.
template <class Ops>
bool copyPartBank(Ops& ops, int part, const std::string& srcDir, const std::string& dstDir)
{
    static_assert(kBankFiles <= 32, "one bit per file");
    uint32_t absent = 0;
    for (int i = 0; i < kBankFiles; i++)
    {
        const std::string file = bankFileName(part, i);
        const CopyResult r = ops.copy(srcDir + "\\" + file, dstDir + "\\" + file);
        if (r == CopyResult::kFailed)
        {
            return false;
        }
        if (r == CopyResult::kAbsent && i > 0)
        {
            absent |= 1u << i;
        }
    }
    if (absent == 0)
    {
        return true;
    }
    if (!ops.sourceReadable())
    {
        return false; // "not there" may be the card failing, nothing is removed on that word
    }
    for (int i = 1; i < kBankFiles; i++)
    {
        if (absent & (1u << i))
        {
            const std::string dst = dstDir + "\\" + bankFileName(part, i);
            if (ops.exists(dst) && !ops.remove(dst))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace polysynth::open
