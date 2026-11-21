#pragma once
#include "input.hpp"

// Give each spelling a dense integer in its hash partition before touching the
// graph. The serial union pass then operates entirely on integer arrays.
class Identities {
    static constexpr size_t parts=256;
    struct Edge { u32 ends[2]; };
    struct Endpoint { u64 key; u32 edge, side; };
    using Table=FlatMap<u64>;
    struct Slot { u64 key=0; u32 canonical=0; };
    struct Shard {
        std::vector<Slot> slots;
        size_t probes=32;
        std::mutex mutex;
        std::unordered_map<u64,u32> overflow;
        std::vector<u64> extra_names;
    };
    std::array<Shard,parts> shards;
    static u32 code(size_t b,size_t slot) { return u32((slot+1)*parts+b); }
public:
    Identities(const Inputs& input, Keys& users, int threads,size_t event_bytes) {
        std::array<Table,parts> tables;
        std::vector<std::vector<std::array<u64,2>>> pairs(threads);
        parallel(input.blocks.size(),threads,[&](int t,size_t i) {
            rows<2>(input.blocks[i],[&](const auto& f) {
                if (!f[0].empty() && !f[1].empty()) pairs[t].push_back({users.get(f[0]),users.get(f[1])});
            });
        });
        std::vector<u32> offset(threads+1);
        for (int t=0;t<threads;t++) offset[t+1]=offset[t]+pairs[t].size();
        std::vector<Edge> edges(offset.back());
        std::vector<std::array<std::vector<Endpoint>,parts>> endpoints(threads);
        parallel(threads,threads,[&](int,size_t t) {
            for (u32 j=0;j<pairs[t].size();j++) for (u32 side=0;side<2;side++) {
                u64 key=pairs[t][j][side];
                endpoints[t][mix(key)>>56].push_back({key,offset[t]+j,side});
            }
        });
        std::array<u32,parts+1> base{};
        for (size_t b=0;b<parts;++b) {
            base[b+1]=base[b];
            for (auto& local:endpoints) base[b+1]+=local[b].size();
        }
        std::vector<u32> parent(base.back());
        std::vector<u64> names(base.back());
        parallel(parts,threads,[&](int,size_t b) {
            u32 next=base[b];
            auto& tbl=tables[b];
            tbl=Table((base[b+1]-base[b])*2/3+1);
            for (auto& local:endpoints) for (auto& e:local[b]) {
                auto& s=tbl.insert(e.key,next);
                if (s.value==next) { parent[next]=next; names[next++]=e.key; }
                edges[e.edge].ends[e.side]=s.value;
            }
        });
        auto root=[&](u32 n) {
            while (parent[n]!=n) { parent[n]=parent[parent[n]]; n=parent[n]; }
            return n;
        };
        for (auto& e:edges) {
            auto a=root(e.ends[0]), b=root(e.ends[1]);
            if (a!=b) {
                if (users.less(names[b],names[a])) std::swap(a,b);
                parent[b]=a;
            }
        }
        // Root discovery is read-only here so independent partitions can finish
        // concurrently even when a shared device links IDs from many of them.
        parallel(parts,threads,[&](int,size_t b) {
            for (auto& s:tables[b].slots) if (s.key) {
                u32 n=s.value;
                while (parent[n]!=n) n=parent[n];
                s.value=names[n];
            }
        });
        // Reserve spare slots for users that only appear in event or click
        // files. Capacity estimates change memory use only. Overflow remains exact.
        parallel(parts,threads,[&](int,size_t b) {
            auto& part=shards[b];
            part.slots.resize(capacity(tables[b].used+event_bytes/(1800*parts)+8));
            size_t mask=part.slots.size()-1;
            for (auto& entry:tables[b].slots) if (entry.key) {
                size_t i=mix(entry.key)&mask, steps=1;
                while (part.slots[i].key) { i=(i+1)&mask; ++steps; }
                part.slots[i].key=entry.key;
                part.probes=std::max(part.probes,steps);
            }
            part.probes=std::min(part.probes,part.slots.size());
        });
        parallel(parts,threads,[&](int,size_t b) {
            for (auto& slot:shards[b].slots) if (slot.key) {
                u64 canonical=tables[b].locate(slot.key).value;
                u64 h=mix(canonical);
                auto& target=shards[h>>56].slots;
                size_t i=h&(target.size()-1);
                while (target[i].key!=canonical) i=(i+1)&(target.size()-1);
                slot.canonical=code(h>>56,i);
            }
        });
    }
    void prefetch(u64 key) const {
        u64 h=mix(key); auto& v=shards[h>>56].slots;
        __builtin_prefetch(&v[h&(v.size()-1)]);
    }
    u32 person(u64 key) {
        u64 h=mix(key); size_t b=h>>56;
        auto& part=shards[b]; auto& v=part.slots;
        size_t mask=v.size()-1, i=h&mask;
        for (size_t p=0;p<part.probes;p++,i=(i+1)&mask) {
            std::atomic_ref<u64> raw(v[i].key);
            u64 found=raw.load(std::memory_order_relaxed);
            if (!found && raw.compare_exchange_strong(found,key,std::memory_order_relaxed))
                return code(b,i);
            if (found==key) return v[i].canonical?v[i].canonical:code(b,i);
        }
        std::lock_guard<std::mutex> guard(part.mutex);
        auto [it,inserted]=part.overflow.try_emplace(key,code(b,v.size()+part.extra_names.size()));
        if (inserted) part.extra_names.push_back(key);
        return it->second;
    }
    size_t extent(size_t b) const { return shards[b].slots.size()+shards[b].extra_names.size(); }
    u64 name(size_t b,size_t slot) const {
        const auto& part=shards[b];
        return slot<part.slots.size()?part.slots[slot].key:part.extra_names[slot-part.slots.size()];
    }
};
