#include "Merge.h"

#include "Bmg.h"
#include "Rarc.h"
#include "Sarc.h"
#include "Yaz0.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace merge {

namespace {

const size_t kMaxConflictPaths = 200;
// Nested archives deeper than this are treated as opaque files.
const int kMaxDepth = 8;

void addConflict(Result &r, const std::string &path) {
    r.conflicts++;
    if (r.conflictPaths.size() < kMaxConflictPaths) r.conflictPaths.push_back(path);
}

std::string mergeData(const std::string *base, const std::vector<const std::string *> &mods, const std::string &path,
                      int depth, Result &r);

// Merges archives that each changed something. `base` may be null.
std::string mergeSarc(const sarc::Archive *base, const std::vector<sarc::Archive> &arcs, const std::string &path,
                      int depth, Result &r) {
    sarc::Archive out;
    out.bigEndian = arcs[0].bigEndian;
    out.hashKey = arcs[0].hashKey;

    // Keep the base order, then entries the mods added, in priority order.
    std::vector<std::string> names;
    std::set<std::string> seen;
    if (base)
        for (const auto &f : base->files)
            if (seen.insert(f.name).second) names.push_back(f.name);
    for (const auto &a : arcs)
        for (const auto &f : a.files)
            if (seen.insert(f.name).second) names.push_back(f.name);

    // Name lookup per archive; big packs hold thousands of entries.
    typedef std::unordered_map<std::string, const sarc::File *> Index;
    auto makeIndex = [](const sarc::Archive &a) {
        Index idx;
        idx.reserve(a.files.size());
        for (const auto &f : a.files) idx.emplace(f.name, &f); // first wins, like find()
        return idx;
    };
    auto lookup = [](const Index &idx, const std::string &name) -> const sarc::File * {
        auto it = idx.find(name);
        return it == idx.end() ? nullptr : it->second;
    };
    Index baseIdx;
    if (base) baseIdx = makeIndex(*base);
    std::vector<Index> arcIdx;
    for (const auto &a : arcs) arcIdx.push_back(makeIndex(a));

    for (const auto &name : names) {
        const sarc::File *bf = base ? lookup(baseIdx, name) : nullptr;
        uint32_t align = bf ? bf->alignment : 4;

        // What each archive did to this entry, in priority order.
        enum class Act { Keep, Change, Delete };
        std::vector<Act> acts;
        std::vector<const std::string *> changes;
        for (const auto &idx : arcIdx) {
            const sarc::File *f = lookup(idx, name);
            if (f) {
                align = std::max(align, f->alignment);
                if (bf && f->data == bf->data) {
                    acts.push_back(Act::Keep);
                } else {
                    acts.push_back(Act::Change);
                    changes.push_back(&f->data);
                }
            } else {
                // Without a base, a missing entry is just one this mod lacks.
                acts.push_back(bf ? Act::Delete : Act::Keep);
            }
        }

        Act first = Act::Keep;
        bool hasDelete = false;
        for (Act a : acts) {
            if (a == Act::Delete) hasDelete = true;
            if (first == Act::Keep) first = a;
        }
        std::string entryPath = path + "/" + name;
        if (first == Act::Keep) {
            if (bf) out.files.push_back(*bf);
            continue;
        }
        if (hasDelete && !changes.empty()) {
            // One mod removed the entry, another edited it.
            addConflict(r, entryPath);
            if (first == Act::Delete) continue;
        } else if (first == Act::Delete) {
            continue;
        }

        sarc::File nf;
        nf.name = name;
        nf.data = mergeData(bf ? &bf->data : nullptr, changes, entryPath, depth + 1, r);
        nf.alignment = align;
        out.files.push_back(std::move(nf));
    }
    return sarc::write(out);
}

// Picks the result for one item (a message, a section) that each version may
// have changed: the base if nobody changed it, the change if only one change
// exists, else a conflict won by the highest-priority change.
template <typename T>
const T &pickThreeWay(const T *base, const std::vector<const T *> &versions, const std::string &path, Result &r) {
    const T *first = nullptr;
    bool conflict = false;
    for (const T *v : versions) {
        if (base && *v == *base) continue;
        if (!first) first = v;
        else if (*v != *first) conflict = true;
    }
    if (!first) return base ? *base : *versions[0];
    if (conflict) addConflict(r, path);
    return *first;
}

// RARC: archives with the same tree are merged file by file.
bool mergeRarc(const std::string *base, const std::vector<const std::string *> &changed, const std::string &path,
               int depth, Result &r, std::string &out) {
    std::vector<rarc::Archive> arcs(changed.size());
    for (size_t i = 0; i < changed.size(); ++i) {
        if (!rarc::parse(*changed[i], arcs[i])) return false;
        if (i && !rarc::sameLayout(arcs[0], arcs[i])) return false;
    }
    rarc::Archive baseArc;
    bool haveBase = base && rarc::isRarc(*base) && rarc::parse(*base, baseArc) && rarc::sameLayout(arcs[0], baseArc);

    std::vector<std::string> data(arcs[0].entries.size());
    for (size_t e = 0; e < arcs[0].entries.size(); ++e) {
        if (!arcs[0].entries[e].isFile()) continue;
        std::vector<std::string> copies(arcs.size());
        std::vector<const std::string *> ptrs;
        for (size_t i = 0; i < arcs.size(); ++i) {
            copies[i] = arcs[i].fileData(e);
            ptrs.push_back(&copies[i]);
        }
        std::string baseData;
        if (haveBase) baseData = baseArc.fileData(e);
        data[e] = mergeData(haveBase ? &baseData : nullptr, ptrs, path + "/" + arcs[0].entries[e].name, depth + 1, r);
    }
    out = rarc::rebuild(arcs[0], data);
    r.combined = true;
    return true;
}

// BMG: messages are merged one by one. Without the original file every
// differing message is a conflict, so priority decides as before.
bool mergeBmg(const std::string *base, const std::vector<const std::string *> &changed, const std::string &path,
              Result &r, std::string &out) {
    std::vector<bmg::File> files(changed.size());
    for (size_t i = 0; i < changed.size(); ++i) {
        if (!bmg::parse(*changed[i], files[i])) return false;
        if (i && !bmg::compatible(files[0], files[i])) return false;
    }
    bmg::File baseFile;
    bool haveBase = base && bmg::isBmg(*base) && bmg::parse(*base, baseFile) && bmg::compatible(files[0], baseFile);

    std::vector<bmg::Message> messages;
    messages.reserve(files[0].messages.size());
    for (size_t m = 0; m < files[0].messages.size(); ++m) {
        std::vector<const bmg::Message *> versions;
        for (const auto &f : files) versions.push_back(&f.messages[m]);
        messages.push_back(pickThreeWay(haveBase ? &baseFile.messages[m] : nullptr, versions,
                                        path + "#" + std::to_string(m), r));
    }
    std::vector<std::string> sections(files[0].sections.size());
    for (size_t s = 0; s < sections.size(); ++s) {
        if (files[0].sections[s].empty()) continue; // INF1/DAT1, rebuilt from messages
        std::vector<const std::string *> versions;
        for (const auto &f : files) versions.push_back(&f.sections[s]);
        sections[s] = pickThreeWay(haveBase ? &baseFile.sections[s] : nullptr, versions,
                                   path + "/" + files[0].sectionMagic[s], r);
    }
    out = bmg::rebuild(files[0], messages, sections);
    r.combined = true;
    return true;
}

// Tries a structured merge of `changed` (2+ distinct copies). Returns false
// if the format is not one that can be merged.
bool structuredMerge(const std::string *base, const std::vector<const std::string *> &changed,
                     const std::string &path, int depth, Result &r, std::string &out) {
    if (depth > kMaxDepth) return false;

    // Yaz0: merge the decompressed payloads, then recompress like the
    // highest-priority copy.
    bool anyYaz0 = false;
    for (const auto *c : changed) anyYaz0 |= yaz0::isYaz0(*c);
    if (anyYaz0) {
        std::vector<std::string> plain(changed.size());
        std::vector<const std::string *> ptrs;
        for (size_t i = 0; i < changed.size(); ++i) {
            if (yaz0::isYaz0(*changed[i])) {
                if (!yaz0::decompress(*changed[i], plain[i])) return false;
                ptrs.push_back(&plain[i]);
            } else {
                ptrs.push_back(changed[i]);
            }
        }
        std::string basePlain;
        const std::string *bp = nullptr;
        if (base) {
            if (!yaz0::isYaz0(*base)) bp = base;
            else if (yaz0::decompress(*base, basePlain)) bp = &basePlain;
        }
        std::string merged = mergeData(bp, ptrs, path, depth + 1, r);
        // Reuse an existing compressed copy when the merge picked it whole.
        for (size_t i = 0; i < changed.size(); ++i) {
            if (*ptrs[i] == merged) {
                out = *changed[i];
                return true;
            }
        }
        if (yaz0::isYaz0(*changed[0])) out = yaz0::compress(merged, yaz0::alignmentHint(*changed[0]));
        else out = std::move(merged);
        return true;
    }

    bool allRarc = true, allBmg = true, allSarc = true;
    for (const auto *c : changed) {
        allRarc &= rarc::isRarc(*c);
        allBmg &= bmg::isBmg(*c);
        allSarc &= sarc::isSarc(*c);
    }
    if (allRarc) return mergeRarc(base, changed, path, depth, r, out);
    if (allBmg) return mergeBmg(base, changed, path, r, out);
    if (!allSarc) return false;
    std::vector<sarc::Archive> arcs(changed.size());
    for (size_t i = 0; i < changed.size(); ++i)
        if (!sarc::parse(*changed[i], arcs[i])) return false;
    sarc::Archive baseArc;
    bool haveBase = base && sarc::isSarc(*base) && sarc::parse(*base, baseArc);
    out = mergeSarc(haveBase ? &baseArc : nullptr, arcs, path, depth, r);
    r.combined = true;
    return true;
}

std::string mergeData(const std::string *base, const std::vector<const std::string *> &mods, const std::string &path,
                      int depth, Result &r) {
    // Only copies that differ from the base carry a change.
    std::vector<const std::string *> changed;
    for (const auto *m : mods)
        if (!base || *m != *base) changed.push_back(m);
    if (changed.empty()) return base ? *base : *mods[0];

    // Drop duplicates (two mods that made the same change).
    std::vector<const std::string *> distinct;
    for (const auto *c : changed) {
        bool dup = false;
        for (const auto *d : distinct)
            if (*d == *c) {
                dup = true;
                break;
            }
        if (!dup) distinct.push_back(c);
    }
    if (distinct.size() == 1) return *distinct[0];

    std::string out;
    if (structuredMerge(base, distinct, path, depth, r, out)) return out;
    addConflict(r, path);
    return *distinct[0];
}

} // namespace

Result mergeFiles(const std::string *base, const std::vector<const std::string *> &mods, const std::string &label) {
    Result r;
    if (mods.empty()) return r;
    r.data = mergeData(base, mods, label, 0, r);
    return r;
}

} // namespace merge
