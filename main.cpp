#include "input.hpp"
#include "parquet.hpp"
#include "fast_csv.hpp"
#include "timeparse.hpp"
#include <chrono>
#include <iostream>
#include <numeric>

constexpr size_t bins = 256;
template<class T> using Bins = std::array<std::vector<T>,bins>;
inline size_t bin(u64 k) { return mix(k) >> 56; }

struct Catalog {
    std::vector<std::string> categories, campaigns, channels;
    std::unordered_map<View,u32> products, campaign_index;
    std::deque<std::string> keys;
    u32 unknown;
    Catalog(const fs::path& path) {
        std::unordered_map<std::string,u32> categories_by_name;
        auto category=[&](View label) {
            auto [it,inserted]=categories_by_name.try_emplace(std::string(label),categories.size());
            if (inserted) categories.emplace_back(label);
            return it->second;
        };
        unknown=category("unknown");
        Inputs products_file(path/"products.csv");
        for (auto block:products_file.blocks) rows<2>(block,[&](const auto& f) {
            keys.emplace_back(f[0]); products.emplace(keys.back(),category(f[1]));
        });
        campaigns.emplace_back("organic"); channels.emplace_back("organic");
        Inputs campaign_file(path/"campaigns.csv");
        for (auto block:campaign_file.blocks) rows<2>(block,[&](const auto& f) {
            keys.emplace_back(f[0]); campaign_index.emplace(keys.back(),campaigns.size());
            campaigns.emplace_back(f[0]); channels.emplace_back(f[1]);
        });
    }
};

struct Links {
    struct Node { u32 parent, size; u64 name; };
    FlatMap<u32> ids;
    std::vector<Node> nodes;
    u32 get(u64 id) {
        auto& slot=ids.insert(id,nodes.size());
        if (slot.value==nodes.size()) nodes.push_back({slot.value,1,id});
        return slot.value;
    }
    u32 root(u32 n) {
        while (nodes[n].parent!=n) { n=nodes[n].parent=nodes[nodes[n].parent].parent; }
        return n;
    }
    void join(u64 a,u64 b) {
        u32 x=root(get(a)), y=root(get(b));
        if (x==y) return;
        if (nodes[x].size<nodes[y].size) std::swap(x,y);
        nodes[y].parent=x; nodes[x].size+=nodes[y].size;
    }
    void finish(const Keys& users) {
        for (u32 i=0;i<nodes.size();i++) {
            auto& leader=nodes[root(i)];
            if (users.less(nodes[i].name,leader.name)) leader.name=nodes[i].name;
        }
        for (auto& s:ids.slots) if (s.key) s.value=root(s.value);
    }
    void prefetch(u64 id) const { __builtin_prefetch(&ids.slots[mix(id)&(ids.slots.size()-1)]); }
    u64 person(u64 id) {
        auto& s=ids.locate(id);
        return s.key ? nodes[s.value].name : id;
    }
};

struct Event { u64 id, user; i64 ts, ingested; u32 price, category; };
struct Moment { i64 ts; u64 click; u32 amount, label; };
struct Tagged { u64 person; Moment data; };
struct Window { i64 lo, hi; };
Window manifest(const fs::path& path) {
    std::ifstream in(path);
    std::string body((std::istreambuf_iterator<char>(in)),{});
    auto value=[&](View key) {
        size_t pos=body.find(key);
        if (pos==std::string::npos) throw std::runtime_error("missing manifest key");
        pos=body.find('"',body.find(':',pos)+1)+1;
        return timestamp(View(body).substr(pos,body.find('"',pos)-pos));
    };
    Window w{value("window_start"),value("window_end")};
    if (w.lo==bad_time || w.hi<=w.lo) throw std::runtime_error("invalid window");
    return w;
}

struct InputWorker {
    Bins<Event> events;
    Bins<Tagged> timeline;
    std::vector<Tagged> clicks;
    std::vector<std::pair<u64,u64>> links;
};
struct UserDay { u64 person; int32_t day; i64 sessions, events, revenue; int32_t top; };
struct Totals {
    i64 sessions=0, buyers=0, revenue=0, assists=0;
    void add(const Totals& t) {
        sessions+=t.sessions; buyers+=t.buyers; revenue+=t.revenue; assists+=t.assists;
    }
};
struct CampaignDays {
    int32_t first;
    size_t campaigns;
    std::vector<Totals> dense;
    std::unordered_map<u64,Totals> sparse;
    static u64 key(int32_t day,u32 campaign) { return (u64(u32(day))<<32)|campaign; }
    CampaignDays(int32_t start,int32_t end,size_t count):first(start),campaigns(count) {
        u64 n=u64(i64(end)-start+1)*count;
        if (n<=500'000) dense.resize(n);
    }
    Totals& at(int32_t day,u32 campaign) {
        if (!dense.empty()) return dense[size_t(day-first)*campaigns+campaign];
        return sparse[key(day,campaign)];
    }
    void merge(CampaignDays& other) {
        for (size_t i=0;i<dense.size();i++) dense[i].add(other.dense[i]);
        for (auto& pair:other.sparse) sparse[pair.first].add(pair.second);
    }
};

inline int32_t utc_day(i64 ms) { return int32_t(ms/day_ms-(ms%day_ms<0)); }

class Aggregator {
    const Catalog& catalog;
    const Keys& click_keys;
    std::vector<i64> revenues, latest;
    std::vector<u32> purchases, active;
public:
    std::vector<UserDay> output;
    CampaignDays campaign;
    Aggregator(const Catalog& c,const Keys& k,Window w):
        catalog(c),click_keys(k),revenues(c.categories.size()),latest(c.campaigns.size(),bad_time),
        campaign(utc_day(w.lo),utc_day(w.hi-1),c.campaigns.size()) {}

    void person(u64 id,Moment* start,Moment* end) {
        std::sort(start,end,[](const Moment& a,const Moment& b) {
            if (a.ts!=b.ts) return a.ts<b.ts;
            return bool(a.click)>bool(b.click);
        });
        i64 prev=bad_time, last_click=bad_time;
        u64 best_id=0;
        u32 credited=0;
        UserDay day{id,0,0,0,0,-1};
        Totals* session=nullptr;
        bool purchased=false;
        auto finish_day=[&] {
            i64 best=0;
            for (u32 c:purchases) {
                i64 amount=revenues[c];
                if (amount>best || (amount==best && catalog.categories[c]<catalog.categories[day.top])) {
                    best=amount; day.top=int32_t(c);
                }
                revenues[c]=0;
            }
            purchases.clear();
            output.push_back(day);
        };
        for (auto p=start;p!=end;++p) {
            if (p->click) {
                if (latest[p->label]==bad_time) active.push_back(p->label);
                latest[p->label]=p->ts;
                if (p->ts!=last_click || click_keys.less(p->click,best_id)) {
                    last_click=p->ts; best_id=p->click; credited=p->label;
                }
                continue;
            }
            if (prev==bad_time || p->ts-prev>1'800'000) {
                int32_t date=utc_day(p->ts);
                if (day.sessions && date!=day.day) {
                    finish_day(); day={id,date,0,0,0,-1};
                }
                day.day=date; ++day.sessions; purchased=false;
                i64 floor=p->ts-week_ms;
                session=&campaign.at(date,last_click>=floor ? credited:0);
                ++session->sessions;
                size_t kept=0;
                for (u32 c:active) {
                    if (latest[c]>=floor) {
                        active[kept++]=c;
                        ++campaign.at(date,c).assists;
                    } else latest[c]=bad_time;
                }
                active.resize(kept);
            }
            prev=p->ts; ++day.events;
            if (p->amount) {
                if (!revenues[p->label]) purchases.push_back(p->label);
                revenues[p->label]+=p->amount;
                day.revenue+=p->amount; session->revenue+=p->amount;
                if (!purchased) { ++session->buyers; purchased=true; }
            }
        }
        if (day.sessions) finish_day();
        for (u32 c:active) latest[c]=bad_time;
        active.clear();
    }
};

using parquet::Column;
using parquet::Values;
void user_file(const fs::path& path,const std::vector<UserDay>& rows,const Keys& users,const Catalog& c) {
    parquet::File file(path,{{"user_id",6,0},{"day",1,6},{"sessions",2},{"events",2},
                             {"revenue_cents",2},{"top_category",6,0,true}});
    std::vector<Values> v(6);
    for (size_t pos=0;pos<rows.size();) {
        size_t n=std::min<size_t>(65536,rows.size()-pos);
        u64 previous=0; std::string name;
        for (size_t i=0;i<n;i++) {
            auto& r=rows[pos+i];
            if (r.person!=previous) { previous=r.person; name=users.name(r.person); }
            v[0].text(name); v[1].put(r.day); v[2].put(r.sessions); v[3].put(r.events); v[4].put(r.revenue);
            v[5].defined(i,r.top>=0);
            if (r.top>=0) v[5].text(c.categories[r.top]);
        }
        file.append(n,v); pos+=n;
    }
    file.finish();
}
void campaign_file(const fs::path& path,const CampaignDays& days,const Catalog& c) {
    parquet::File file(path,{{"campaign_id",6,0},{"channel",6,0},{"day",1,6},{"sessions",2},
                             {"purchasing_sessions",2},{"revenue_cents",2},{"assisted_sessions",2}});
    std::vector<Values> v(7); int count=0;
    auto emit=[&](int32_t day,u32 id,const Totals& r) {
        if (!r.sessions && !r.assists) return;
        v[0].text(c.campaigns[id]); v[1].text(c.channels[id]); v[2].put(day);
        v[3].put(r.sessions); v[4].put(r.buyers); v[5].put(r.revenue); v[6].put(r.assists);
        if (++count==65536) { file.append(count,v); count=0; }
    };
    for (size_t i=0;i<days.dense.size();i++) emit(days.first+int32_t(i/days.campaigns),i%days.campaigns,days.dense[i]);
    for (auto& p:days.sparse) emit(int32_t(p.first>>32),u32(p.first),p.second);
    file.append(count,v); file.finish();
}

void run(const fs::path& data,const fs::path& out,int threads) {
    auto then=std::chrono::steady_clock::now();
    auto mark=[&](const char* stage) {
        if (std::getenv("SESSION_PROFILE")) {
            auto now=std::chrono::steady_clock::now();
            std::cerr<<stage<<" "<<std::chrono::duration<double>(now-then).count()<<"s\n"; then=now;
        }
    };
    Window w=manifest(data/"manifest.json");
    Dates dates(w.lo,w.hi);
    Catalog catalog(data);
    Keys users('u'), event_ids('e'), click_ids('k');
    Inputs events(data/"events"), clicks(data/"clicks"), edges(data/"links");
    std::vector<InputWorker> workers(threads);
    size_t bytes=0;
    for (auto block:events.blocks) bytes+=block.end-block.begin;
    size_t per_bin=bytes/(72*threads*bins);
    for (auto& worker:workers) for (auto& part:worker.events) part.reserve(per_bin);
    mark("open");
    parallel(events.blocks.size(),threads,[&](int t,size_t i) {
        fast_rows<7>(events.blocks[i],[&](const auto& f) {
            if (f[0].empty()) return;
            Event e{event_ids.get(f[0]),0,dates.parse(f[2]),dates.parse(f[3]),0,0};
            if (!f[1].empty() && !f[5].empty() && e.ts>=w.lo && e.ts<w.hi && e.ingested-e.ts<=day_ms) {
                if (f[4]=="view" || f[4]=="cart") e.user=users.get(f[1]);
                else if (f[4]=="purchase") {
                    auto price=decimal(f[6],10'000'000);
                    if (price>0) {
                        e.price=price; e.user=users.get(f[1]);
                        auto it=catalog.products.find(f[5]);
                        e.category=it==catalog.products.end()?catalog.unknown:it->second;
                    }
                }
            }
            // Invalid winners are tombstones and still participate in global dedup.
            workers[t].events[bin(e.id)].push_back(e);
        });
    });
    mark("events");
    parallel(clicks.blocks.size(),threads,[&](int t,size_t i) {
        fast_rows<4>(clicks.blocks[i],[&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            i64 ts=dates.parse(f[3]);
            if (ts<w.lo-week_ms || ts>=w.hi) return;
            auto it=catalog.campaign_index.find(f[2]);
            if (it!=catalog.campaign_index.end())
                workers[t].clicks.push_back({users.get(f[1]),{ts,click_ids.get(f[0]),0,it->second}});
        });
    });
    mark("clicks");
    parallel(edges.blocks.size(),threads,[&](int t,size_t i) {
        rows<2>(edges.blocks[i],[&](const auto& f) {
            if (!f[0].empty() && !f[1].empty()) workers[t].links.emplace_back(users.get(f[0]),users.get(f[1]));
        });
    });
    Links links;
    for (auto& worker:workers) for (auto [a,b]:worker.links) links.join(a,b);
    links.finish(users);
    mark("identity");
    parallel(bins,threads,[&](int t,size_t b) {
        size_t count=0;
        for (auto& worker:workers) count+=worker.events[b].size();
        FlatMap<Event*> winners(count);
        for (auto& worker:workers) for (auto& e:worker.events[b]) {
            auto& slot=winners.locate(e.id);
            if (!slot.key) slot={e.id,&e};
            else if (e.ingested<slot.value->ingested) { slot.value->user=0; slot.value=&e; }
            else e.user=0;
        }
        auto& dest=workers[t].timeline;
        for (auto& worker:workers) {
            auto& list=worker.events[b];
            for (size_t j=0;j<list.size();++j) {
                if (j+16<list.size()) links.prefetch(list[j+16].user);
                const auto& e=list[j];
                if (!e.user) continue;
                u64 person=links.person(e.user);
                dest[bin(person)].push_back({person,{e.ts,0,e.price,e.category}});
            }
        }
        for (auto& worker:workers) std::vector<Event>().swap(worker.events[b]);
    });
    parallel(workers.size(),threads,[&](int,size_t i) {
        auto& worker=workers[i];
        for (auto& c:worker.clicks) {
            c.person=links.person(c.person);
            worker.timeline[bin(c.person)].push_back(c);
        }
        std::vector<Tagged>().swap(worker.clicks);
    });
    mark("dedup and partition");
    std::vector<Aggregator> agg;
    for (int i=0;i<threads;i++) agg.emplace_back(catalog,click_ids,w);
    parallel(bins,threads,[&](int t,size_t b) {
        size_t n=0;
        for (auto& worker:workers) n+=worker.timeline[b].size();
        struct Group { u32 end=0, cursor=0; };
        FlatMap<Group> people(std::min<size_t>(n,n/24+16));
        for (auto& worker:workers) for (auto& p:worker.timeline[b]) ++people.insert(p.person,{}).value.end;
        u32 pos=0;
        for (auto& s:people.slots) if (s.key) {
            s.value.cursor=pos; pos+=s.value.end; s.value.end=pos;
        }
        std::unique_ptr<Moment[]> ordered(new Moment[n]);
        for (auto& worker:workers) {
            for (auto& p:worker.timeline[b]) {
                auto& g=people.locate(p.person).value;
                ordered[g.cursor++]=p.data;
            }
            std::vector<Tagged>().swap(worker.timeline[b]);
        }
        pos=0;
        for (auto& s:people.slots) if (s.key) {
            agg[t].person(s.key,ordered.get()+pos,ordered.get()+s.value.end);
            pos=s.value.end;
        }
    });
    mark("sessions");
    fs::create_directories(out/"user_daily"); fs::create_directories(out/"campaign_daily");
    parallel(agg.size(),threads,[&](int,size_t i) {
        if (i==0 || !agg[i].output.empty())
            user_file(out/"user_daily"/("part-"+std::to_string(i)+".parquet"),agg[i].output,users,catalog);
    });
    for (size_t i=1;i<agg.size();i++) agg[0].campaign.merge(agg[i].campaign);
    campaign_file(out/"campaign_daily"/"part-0.parquet",agg[0].campaign,catalog);
    mark("parquet");
}

int main(int argc,char** argv) {
    try {
        auto env=[](const char* key,const char* fallback) {
            const char* val=std::getenv(key); return val?val:fallback;
        };
        run(argc>1?argv[1]:env("DATA_DIR","/data"),argc>2?argv[2]:env("OUTPUT_DIR","/output"),
            std::clamp(std::atoi(env("NCPU","8")),1,64));
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
