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
// `prune` false: copy what the source has and delete nothing (the source is a Workspace that may lack files only because an earlier copy
// into it failed, see copyBank).
template <class Ops>
bool copyPartBank(Ops& ops, int part, const std::string& srcDir, const std::string& dstDir, bool prune = true)
{
    static_assert(kBankFiles <= 32, "one bit per file");
    uint32_t absent = 0;
    for (int i = 0; i < kBankFiles; i++)
    {
        const std::string file = bankFileName(part, i);
        const std::string src = srcDir + "\\" + file;
        const std::string* from = &src;
        std::string bak; // only set when the source file is gone: the copy then comes from <file>.bak
        Presence p = ops.stat(src);
        if (p == Presence::kAbsent)
        {
            bak = src + ".bak";
            const Presence b = ops.stat(bak);
            if (b == Presence::kPresent)
            {
                from = &bak;
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
        if (!ops.copyFile(*from, dstDir + "\\" + file))
        {
            return false;
        }
    }
    for (int i = 1; prune && i < kBankFiles; i++)
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

// The bank copy of all `parts` parts, with the per-session "bank copy incomplete" latch `incomplete` (one bit per part, main thread only).
// `projectOpen` (project folder -> Workspace): the latch of each part is set when its copy failed and cleared when it worked, so the
// latch always describes the last open. Otherwise (project save, Workspace -> project folder): a part whose latch is set may lack slot
// files in the Workspace only because the open copy failed, the card still has them in Projects, so that part is copied and nothing is
// pruned in Projects. The latch is not cleared by a save: a save to another folder (save as) says nothing about the opened project,
// it is cleared by the next project open that copies the part completely or by a new project (clearAll). Returns false when any part's
// copy failed.
template <class Ops>
bool copyBank(Ops& ops, int parts, const std::string& srcDir, const std::string& dstDir, bool projectOpen, uint32_t& incomplete)
{
    bool all = true;
    for (int part = 0; part < parts; part++)
    {
        const uint32_t bit = 1u << part;
        const bool ok = copyPartBank(ops, part, srcDir, dstDir, projectOpen || !(incomplete & bit));
        if (projectOpen)
        {
            incomplete = ok ? (incomplete & ~bit) : (incomplete | bit);
        }
        all = ok && all;
    }
    return all;
}

// Project open, after the project's own files are in the Workspace: copies the patch bank, then loads the requested project.
// As in stock the result of the bank copy never decides the load, a patch error is not a reason to dispose of the copied song, patterns
// and samples (a new empty project would): the copy is tried once more (it replaces files whole, so it is safe to run again) and the
// project loads with the patch files the Workspace has. Only a Workspace that cannot be opened at all falls back to a new project.
// `settle` runs between the copy and the first load attempt. The audio engine restarts only in the normal load.
template <class Copy, class Settle, class Open, class CreateNew>
void openProjectAfterBankCopy(Copy copy, Settle settle, Open open, CreateNew createNew)
{
    if (!copy())
    {
        copy();
    }
    settle();
    if (!open())
    {
        createNew();
        open();
    }
}

} // namespace polysynth::open
