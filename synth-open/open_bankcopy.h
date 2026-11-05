// Open PolySynth shim for MK66 - copy of the patch bank files of a part between two folders (project save and load).
// Plain C++ over a small file operations type, so the host smoke test runs the same code with an in-memory card that fails on request.
#pragma once

#include "open_fileops.h"
#include "open_load.h"

#include <cstdint>
#include <string>

namespace polysynth::open
{

// The files of a part: its slot files 0..15, the slot 0 backup and the slot names, in this order. Name without folder.
constexpr int kBankFiles = kBackupSlot + 2;
inline std::string bankFileName(int part, int index)
{
    char name[8];
    patchSlotName(part, index <= kBackupSlot ? index : 0, name);
    return std::string(name) + (index <= kBackupSlot ? ".patch" : kSlotNamesExt);
}

// Copies the bank files of `part` from `srcDir` to `dstDir`. `Ops` provides
//   Presence stat(const std::string& path)   kAbsent only when the card found no such entry, kUnknown on any card error (never a failed lookup
//                                            taken for "not there": it would delete a saved file)
//   bool copyFile(const std::string& src, const std::string& dst)   the whole file replaces dst or dst stays as it is (false)
//   bool remove(const std::string& path)
// A source file that is not there but has an old version at <file>.bak (an interrupted replace) is copied from the .bak. Every file that
// exists in the source replaces the destination file only when it was copied completely. A file the source does not have (the card
// said so for the file and for its .bak) is removed from the destination, with its .bak (a slot never survives from another project),
// but only after every copy worked and only when the destination's own state is known. Slot 0 is never removed (stock firmware knows
// only that file). False: some file could not be copied or removed, or the card could not say, the caller reports it and nothing has been
// removed on a guess. The files that were not touched are as they were.
template <class Ops>
bool copyPartBank(Ops& ops, int part, const std::string& srcDir, const std::string& dstDir)
{
    static_assert(kBankFiles <= 32, "one bit per file");
    uint32_t absent = 0;
    for (int i = 0; i < kBankFiles; i++)
    {
        const std::string file = bankFileName(part, i);
        const std::string src = srcDir + "\\" + file;
        std::string from = src;
        Presence p = ops.stat(src);
        if (p == Presence::kAbsent)
        {
            const std::string bak = src + ".bak";
            const Presence b = ops.stat(bak);
            if (b == Presence::kPresent)
            {
                from = bak;
                p = Presence::kPresent;
            }
            else if (b == Presence::kUnknown)
            {
                p = Presence::kUnknown;
            }
        }
        if (p == Presence::kUnknown)
        {
            return false;
        }
        if (p == Presence::kAbsent)
        {
            if (i > 0)
            {
                absent |= 1u << i;
            }
            continue;
        }
        if (!ops.copyFile(from, dstDir + "\\" + file))
        {
            return false;
        }
    }
    for (int i = 1; i < kBankFiles; i++)
    {
        if (!(absent & (1u << i)))
        {
            continue;
        }
        const std::string dst = dstDir + "\\" + bankFileName(part, i);
        // the .bak first: a power loss in between leaves a file that the next save prunes again, never a .bak that a reader brings back
        for (const std::string& path : { dst + ".bak", dst })
        {
            const Presence p = ops.stat(path);
            if (p == Presence::kUnknown || (p == Presence::kPresent && !ops.remove(path)))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace polysynth::open
