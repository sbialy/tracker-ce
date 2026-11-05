// Open PolySynth shim for MK66 - copy of the patch bank files of a part between two folders (project save and load).
// Plain C++ over a small file operations type, so the host smoke test runs the same code with an in-memory card that fails on request.
#pragma once

#include "open_fileops.h"
#include "open_load.h"

#include <cstdint>
#include <cstring>
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
// One bit per bank file (the index of bankFileName), the set of files of a part
constexpr uint32_t kBankAllFiles = (1u << kBankFiles) - 1;
static_assert(kBankFiles <= 32, "one bit per file");

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
// Every file is tried, a failure does not stop the others. `failed` (if given) gets the files whose copy failed or whose lookup in the
// source was unknown: exactly the files that are missing in the destination because of the card (the bank copy marker lists them).
// `protect`: the files of the part that must stay as they are in the destination, never replaced, removed or looked up there (the
// slots a failed project open left out of the Workspace: the original is in the project folder, whatever the Workspace has under that name
// was not read from it). A protected slot file 0..15 that the source does have (a file that was made in its place) makes the result false,
// the caller reports that it was not saved. A protected backup or names file is skipped quietly.
template <class Ops>
bool copyPartBank(Ops& ops, int part, const std::string& srcDir, const std::string& dstDir, uint32_t protect = 0, uint32_t* failed = nullptr)
{
    uint32_t absent = 0;
    uint32_t bad = 0;
    bool ok = true;
    for (int i = 0; i < kBankFiles; i++)
    {
        const uint32_t bit = 1u << i;
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
        if (protect & bit)
        {
            if (i < kPatchBankSlots && p != Presence::kAbsent)
            {
                ok = false; // there is a file here that must not go over the original
            }
            continue;
        }
        if (p == Presence::kUnknown)
        {
            bad |= bit;
            continue;
        }
        if (p == Presence::kAbsent)
        {
            if (i > 0)
            {
                absent |= bit;
            }
            continue;
        }
        if (!ops.copyFile(*from, dstDir + "\\" + file))
        {
            bad |= bit;
        }
    }
    if (failed)
    {
        *failed = bad;
    }
    if (bad)
    {
        return false; // nothing is pruned on a part that could not be copied whole
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
    return ok;
}

// Which files of each part a failed project open did not copy into the Workspace: bit i of part[p] is bank file i (a slot, the backup or
// the names). The Workspace has no such file, or has one that was made after the failed open, and neither is the project's own, so the
// project folder's file must not be pruned, replaced or taken for an empty slot. Persistent: the marker file in the Workspace
// (bankMarkerBegin and bankMarkerFinish write it, bankMarkerLoad brings it back at boot and at project open).
// `ramOnly`: the marker could not be written, the masks are only here and the load must not replace them from the card.
struct BankMissing
{
    uint32_t part[kPatchBankParts] = {};
    bool ramOnly = false;
};
// Slots 1..15 that the project folder has and the Workspace lacks because of the card: the selector refuses them (no empty slot, no copy of
// the open sound, no file). Slot 0 is not refused: it loads (or falls back) as in stock, only its project file is protected.
inline bool slotUnavailable(const BankMissing& m, int part, int slot)
{
    return part >= 0 && part < kPatchBankParts && slot >= 1 && slot < kPatchBankSlots && ((m.part[part] >> slot) & 1u);
}

// The bank copy of all `parts` parts. `projectOpen` (project folder -> Workspace): the masks of `m` are the files of each part that this
// copy could not bring over (a copy that works leaves 0). Otherwise (project save, Workspace -> project folder): the files of `m` are
// protected, nothing of them is replaced or removed in the project folder, the rest is copied and pruned as usual. A save never changes
// `m`: it is cleared by a project open that copies every file or by a new project (bankMarkerFinish, bankMarkerClear). Save as to another
// project name: the protected files are simply not in the new project, the original project keeps them. False when any part could not be
// copied or a protected slot has a file in the Workspace that could not be saved.
template <class Ops>
bool copyBank(Ops& ops, int parts, const std::string& srcDir, const std::string& dstDir, bool projectOpen, BankMissing& m)
{
    bool all = true;
    for (int part = 0; part < parts; part++)
    {
        uint32_t failed = 0;
        const bool ok = copyPartBank(ops, part, srcDir, dstDir, projectOpen ? 0u : m.part[part], &failed);
        if (projectOpen)
        {
            m.part[part] = failed;
        }
        all = ok && all;
    }
    return all;
}

// The marker file (Workspace/bankcopy.missing): "tffmissing 1", then one line "<part>=<hex mask>" for each part that has a missing file.
constexpr size_t kBankMarkerMax = 64;
constexpr char kBankMarkerFile[] = "bankcopy.missing";
inline size_t bankMarkerEncode(const BankMissing& m, uint8_t (&out)[kBankMarkerMax])
{
    static constexpr char kHex[] = "0123456789abcdef";
    size_t n = 0;
    for (const char c : "tffmissing 1\n")
    {
        if (c)
        {
            out[n++] = uint8_t(c);
        }
    }
    for (int part = 0; part < kPatchBankParts; part++)
    {
        const uint32_t mask = m.part[part] & kBankAllFiles;
        if (!mask)
        {
            continue;
        }
        out[n++] = uint8_t('0' + part);
        out[n++] = uint8_t('=');
        bool started = false;
        for (int shift = 28; shift >= 0; shift -= 4)
        {
            const uint32_t d = (mask >> shift) & 15u;
            if (d || started || shift == 0)
            {
                out[n++] = uint8_t(kHex[d]);
                started = true;
            }
        }
        out[n++] = uint8_t('\n');
    }
    return n;
}
// False for anything that is not a marker of this version (the load then guards everything)
inline bool bankMarkerParse(const uint8_t* data, size_t n, BankMissing& m)
{
    static constexpr char kHead[] = "tffmissing 1\n";
    constexpr size_t kHeadLen = sizeof(kHead) - 1;
    uint32_t masks[kPatchBankParts] = {};
    if (n < kHeadLen || std::memcmp(data, kHead, kHeadLen) != 0)
    {
        return false;
    }
    size_t i = kHeadLen;
    while (i < n)
    {
        if (data[i] < '0' || data[i] >= '0' + kPatchBankParts || i + 1 >= n || data[i + 1] != '=')
        {
            return false;
        }
        const int part = data[i] - '0';
        i += 2;
        uint32_t mask = 0;
        size_t digits = 0;
        for (; i < n && data[i] != '\n'; i++, digits++)
        {
            const uint8_t c = data[i];
            const uint32_t d = c >= '0' && c <= '9' ? uint32_t(c - '0') : c >= 'a' && c <= 'f' ? uint32_t(c - 'a' + 10) : 16u;
            if (d > 15 || digits >= 8)
            {
                return false;
            }
            mask = (mask << 4) | d;
        }
        if (i >= n || digits == 0 || (mask & ~kBankAllFiles))
        {
            return false;
        }
        masks[part] = mask;
        i++; // the newline
    }
    for (int part = 0; part < kPatchBankParts; part++)
    {
        m.part[part] = masks[part];
    }
    return true;
}

// Main thread, open_cold.cpp: the card side of the marker in the Workspace store, file bankcopy.missing. Load: the bytes (at most `cap`), kAbsent
// when the card says there is no such file, kUnknown on a card error. Store: the file becomes exactly these bytes or stays as it was.
// Erase: true when the file (and a leftover .bak or .tmp of it) is gone or was not there.
Presence bankMarkerLoadRaw(PatchStore& workspace, uint8_t* buf, size_t cap, size_t& n);
bool bankMarkerStoreRaw(PatchStore& workspace, const uint8_t* data, size_t n);
bool bankMarkerEraseRaw(PatchStore& workspace);
// `Io` is the card side of the marker (open_cold.cpp over the Workspace store, the host test over a map):
//   Presence load(uint8_t* buf, size_t cap, size_t& n)   the file bytes (at most cap), kAbsent when the card says there is none, kUnknown on a card error
//   bool store(const uint8_t* data, size_t n)             the file becomes exactly these bytes, or stays as it was
//   bool erase()                                          true when the file is gone (or was not there)
// Project open, before the copy: every file of every part is "not copied yet", on the card too, so a power loss or a reset in the middle of
// the copy (the Workspace holds a part of the bank) is remembered as well. When even this cannot be written the masks stay in RAM only.
template <class Io>
void bankMarkerBegin(Io& io, BankMissing& m)
{
    uint8_t buf[kBankMarkerMax];
    for (int part = 0; part < kPatchBankParts; part++)
    {
        m.part[part] = kBankAllFiles;
    }
    m.ramOnly = !io.store(buf, bankMarkerEncode(m, buf));
}
// Project open, after the copy (`m` holds what it could not copy): the marker lists exactly those files, or is gone when there are none.
// A marker that cannot be written: the strictest rule for the session, every file of the parts that failed is protected, RAM only. A
// marker that cannot be removed stays on the card and the load that follows reads it back, so the session is guarded as well.
template <class Io>
void bankMarkerFinish(Io& io, BankMissing& m)
{
    uint8_t buf[kBankMarkerMax];
    bool any = false;
    for (int part = 0; part < kPatchBankParts; part++)
    {
        any = any || m.part[part] != 0;
    }
    if (!any)
    {
        io.erase();
        m.ramOnly = false;
        return;
    }
    m.ramOnly = !io.store(buf, bankMarkerEncode(m, buf));
    if (m.ramOnly)
    {
        for (int part = 0; part < kPatchBankParts; part++)
        {
            m.part[part] = m.part[part] ? kBankAllFiles : 0u;
        }
    }
}
// Workspace load (boot and project open): the masks come from the marker. No marker: nothing is missing. A marker that is damaged or that the
// card cannot read: everything is, the next project open or a new project ends it.
template <class Io>
void bankMarkerLoad(Io& io, BankMissing& m)
{
    if (m.ramOnly)
    {
        return;
    }
    uint8_t buf[kBankMarkerMax];
    size_t n = 0;
    BankMissing read;
    switch (io.load(buf, sizeof(buf), n))
    {
        case Presence::kAbsent:
            break;
        case Presence::kPresent:
            if (!bankMarkerParse(buf, n, read))
            {
                for (int part = 0; part < kPatchBankParts; part++)
                {
                    read.part[part] = kBankAllFiles;
                }
            }
            break;
        default:
            for (int part = 0; part < kPatchBankParts; part++)
            {
                read.part[part] = kBankAllFiles;
            }
            break;
    }
    for (int part = 0; part < kPatchBankParts; part++)
    {
        m.part[part] = read.part[part];
    }
}
// A new project: the Workspace is empty and complete.
template <class Io>
void bankMarkerClear(Io& io, BankMissing& m)
{
    for (int part = 0; part < kPatchBankParts; part++)
    {
        m.part[part] = 0;
    }
    m.ramOnly = false;
    io.erase();
}

// Project open, after the project's own files are in the Workspace: copies the patch bank, then loads the requested project.
// As in stock the result of the bank copy never decides the load, a patch error is not a reason to dispose of the copied song, patterns
// and samples (a new empty project would): the copy is tried once more (it replaces files whole, so it is safe to run again) and the
// project loads with the patch files the Workspace has. Only a Workspace that cannot be opened at all falls back to a new project.
// `begin` runs before the copy (bankMarkerBegin), `finish` after the last attempt (bankMarkerFinish), `settle` runs between the copy and
// the first load attempt. The audio engine restarts only in the normal load.
template <class Begin, class Copy, class Finish, class Settle, class Open, class CreateNew>
void openProjectAfterBankCopy(Begin begin, Copy copy, Finish finish, Settle settle, Open open, CreateNew createNew)
{
    begin();
    if (!copy())
    {
        copy();
    }
    finish();
    settle();
    if (!open())
    {
        createNew();
        open();
    }
}

} // namespace polysynth::open
