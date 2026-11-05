// Open PolySynth shim for MK66 - replacing a file on the card, and finding it again after an interruption.
// Plain C++ over a small file operations type, so the host smoke test runs the same code on an in-memory card with FatFs rename rules that
// fails (or loses power) at any step.
#pragma once

#include "PolySynth/FileSystem.h"

#include <cstdio>

namespace polysynth::open
{

using Presence = polysynth::FileSystem::Presence;

// `to` becomes the file `from`. FatFs does not rename over a file, so the old file steps aside as <to>.bak and goes back when `from` cannot
// take its place. Ops provides
//   Presence stat(const char* path)    kAbsent only when the card found no such entry, kUnknown on any card error
//   bool remove(const char* path)
//   bool rename(const char* from, const char* to)    fails when `to` exists
// This is not atomic against power loss: between the two renames `to` is absent and the old file is <to>.bak, the new one is `from`.
// Every reader of these files calls recoverBackupOps first (OpenPatchStore) and moves <to>.bak back, so the old file is what is loaded
// and nothing is written over it before that. `from` is never promoted by a reader: a write that was not acknowledged may be half done.
// False: nothing was replaced. `to` is as it was, or absent with the old file at <to>.bak (a failed move back, power loss).
template <class Ops>
bool replaceFileOps(Ops& ops, const char* from, const char* to)
{
    constexpr size_t kPathMax = 128;
    char bak[kPathMax];
    if (std::snprintf(bak, sizeof(bak), "%s.bak", to) >= static_cast<int>(sizeof(bak)))
    {
        return false;
    }
    const Presence old = ops.stat(to);
    if (old == Presence::kUnknown)
    {
        return false; // the card does not say whether there is an old file: touch nothing
    }
    const bool hadOld = old == Presence::kPresent;
    if (hadOld)
    {
        // a .bak next to a canonical file is an older version (the delete after an earlier replace failed)
        const Presence stale = ops.stat(bak);
        if (stale == Presence::kUnknown || (stale == Presence::kPresent && !ops.remove(bak)))
        {
            return false;
        }
        if (!ops.rename(to, bak))
        {
            return false;
        }
    }
    if (!ops.rename(from, to))
    {
        if (hadOld)
        {
            ops.rename(bak, to); // when this fails too, `to` is absent and the old file stays at <to>.bak for recoverBackupOps
        }
        return false;
    }
    if (ops.stat(bak) == Presence::kPresent)
    {
        ops.remove(bak); // best effort: the old file, or an orphan of an earlier failed move back, is not needed any more
    }
    return true;
}

// The file `path` is absent and an interrupted replace left the old one at <path>.bak: moves it back.
// kPresent: `path` is there (it was, or it is now). kAbsent: it is not there and there is nothing to restore. kUnknown: the card
// failed or the move back did, the file may exist: the caller must treat it as an error, not as an empty slot.
template <class Ops>
Presence recoverBackupOps(Ops& ops, const char* path)
{
    const Presence now = ops.stat(path);
    if (now != Presence::kAbsent)
    {
        return now;
    }
    constexpr size_t kPathMax = 128;
    char bak[kPathMax];
    if (std::snprintf(bak, sizeof(bak), "%s.bak", path) >= static_cast<int>(sizeof(bak)))
    {
        return Presence::kAbsent;
    }
    const Presence b = ops.stat(bak);
    if (b != Presence::kPresent)
    {
        return b; // kAbsent: nothing to restore, kUnknown: it may be there
    }
    return ops.rename(bak, path) ? Presence::kPresent : Presence::kUnknown;
}

} // namespace polysynth::open
