#include "input.hpp"
#include "parquet.hpp"
#include "fast_csv.hpp"
#include "timeparse.hpp"
#include "identities.hpp"
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
    std::vector<u32> product_numbers, campaign_numbers;
    static u32 number(View s,size_t prefix) {
        if (s.size()<=prefix || s.size()-prefix>7 || (s[prefix]=='0' && s.size()>prefix+1)) return UINT32_MAX;
        u32 value=0;
        for (size_t i=prefix;i<s.size();i++) {
            u32 d=(unsigned char)s[i]-unsigned('0');
            if (d>9) return UINT32_MAX;
            value=value*10+d;
        }
        return value;
    }
    u32 campaign(View key) const {
        if (key.starts_with("c_")) {
            u32 n=number(key,2);
            if (n<campaign_numbers.size()) return campaign_numbers[n];
        }
        auto it=campaign_index.find(key);
        return it==campaign_index.end()?0:it->second;
    }
    u32 category_of(View key) const {
        if (key.starts_with("p")) {
            u32 n=number(key,1);
            if (n<product_numbers.size()) return product_numbers[n];
        }
        auto it=products.find(key);
        return it==products.end()?unknown:it->second;
    }
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
        product_numbers.assign(products.size()*2+1,unknown);
        for (auto& [key,cat]:products) if (key.starts_with("p")) {
            u32 n=number(key,1);
            if (n<product_numbers.size()) product_numbers[n]=cat;
        }
        campaign_numbers.resize(campaign_index.size()*2+1);
        for (auto& [key,id]:campaign_index) if (key.starts_with("c_")) {
            u32 n=number(key,2);
            if (n<campaign_numbers.size()) campaign_numbers[n]=id;
        }
    }
};


template<class Time> struct Moment { u64 value; Time ts; u32 label; };
template<class Time> struct Record { u64 ingest_or_click; Time ts; u32 person, amount, label; };
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

// Appending to one shard never moves an earlier record. A duplicate receipt
// therefore needs only a key and a pointer, and only losers are touched again.
template<class T> class Pages {
    static constexpr size_t n=256;
    std::vector<std::unique_ptr<T[]>> pages;
    size_t count=0;
public:
    T* append(const T& record) {
        if (!(count% n)) pages.emplace_back(new T[n]);
        auto* dst=&pages.back()[count++%n]; *dst=record; return dst;
    }
    size_t size() const { return count; }
    template<class F> void each(F fn) {
        size_t left=count;
        for (auto& page:pages) {
            size_t take=std::min(n,left);
            for (size_t i=0;i<take;i++) fn(page[i]);
            left-=take;
        }
    }
    void release() { pages.clear(); count=0; }
};
template<class Time> struct Receipt { u64 id; Record<Time>* record; };
template<class Time> struct InputWorker {
    Bins<Receipt<Time>> receipts;
    std::array<Pages<Record<Time>>,bins> timeline;
};
constexpr u32 click_flag=1u<<31;
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

    template<class Time> void person(u64 id,Moment<Time>* start,Moment<Time>* end,i64 base) {
        auto key=[](const Moment<Time>& m) { return (u64(m.ts)<<1) | !(m.label&click_flag); };
        // Large shared-device and high-activity histories are bandwidth bound.
        // Three stable 11-bit passes cover the common 32-bit manifest offset.
        if constexpr (sizeof(Time)==4) {
            if (end-start>=2048) {
                thread_local std::vector<Moment<Time>> scratch;
                scratch.resize(end-start);
                std::array<std::array<u32,2048>,3> counts{};
                for (auto p=start;p!=end;++p) {
                    u64 k=key(*p);
                    ++counts[0][k&2047]; ++counts[1][(k>>11)&2047]; ++counts[2][k>>22];
                }
                auto* src=start;
                auto* dst=scratch.data();
                for (unsigned pass=0;pass<3;pass++) {
                    u32 offset=0;
                    for (auto& count:counts[pass]) { auto n=count; count=offset; offset+=n; }
                    for (size_t j=0;j<scratch.size();j++) {
                        auto v=src[j]; dst[counts[pass][(key(v)>>(pass*11))&2047]++]=v;
                    }
                    std::swap(src,dst);
                }
                start=src; end=src+scratch.size();
            } else std::sort(start,end,[&](const auto& a,const auto& b) { return key(a)<key(b); });
        } else std::sort(start,end,[&](const auto& a,const auto& b) { return key(a)<key(b); });
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
            i64 ts=base+i64(p->ts);
            u32 label=p->label&~click_flag;
            if (p->label&click_flag) {
                if (latest[label]==bad_time) active.push_back(label);
                latest[label]=ts;
                if (ts!=last_click || click_keys.less(p->value,best_id)) {
                    last_click=ts; best_id=p->value; credited=label;
                }
                continue;
            }
            if (prev==bad_time || ts-prev>1'800'000) {
                int32_t date=utc_day(ts);
                if (day.sessions && date!=day.day) {
                    finish_day(); day={id,date,0,0,0,-1};
                }
                day.day=date; ++day.sessions; purchased=false;
                i64 floor=ts-week_ms;
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
            prev=ts; ++day.events;
            if (p->value) {
                if (!revenues[label]) purchases.push_back(label);
                revenues[label]+=p->value;
                day.revenue+=p->value; session->revenue+=p->value;
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

template<class Time> void run(const fs::path& data,const fs::path& out,int threads,Window w) {
    auto then=std::chrono::steady_clock::now();
    auto mark=[&](const char* stage) {
        if (std::getenv("SESSION_PROFILE")) {
            auto now=std::chrono::steady_clock::now();
            std::cerr<<stage<<" "<<std::chrono::duration<double>(now-then).count()<<"s\n"; then=now;
        }
    };
    i64 base=w.lo-week_ms;
    Dates dates(w.lo,w.hi);
    Catalog catalog(data);
    Keys users('u'), event_ids('e'), click_ids('k');
    Inputs events(data/"events"), clicks(data/"clicks"), edges(data/"links");
    std::vector<InputWorker<Time>> workers(threads);
    size_t bytes=0;
    for (auto block:events.blocks) bytes+=block.end-block.begin;
    size_t per_bin=bytes/(72*threads*bins);
    for (auto& worker:workers) for (auto& part:worker.receipts) part.reserve(per_bin);
    mark("open");
    Identities links(edges,users,threads,bytes);
    mark("identity");
    parallel(events.blocks.size(),threads,[&](int t,size_t i) {
        auto& worker=workers[t];
        struct Pending { u64 id, user; Record<Time> row; };
        std::array<Pending,32> batch;
        size_t pending=0;
        auto flush=[&] {
            for (size_t j=0;j<pending;j++) {
                auto& item=batch[j];
                auto& person=item.row.person;
                person=item.user?links.person(item.user):0;
                auto* record=worker.timeline[person&(bins-1)].append(item.row);
                worker.receipts[bin(item.id)].push_back({item.id,record});
            }
            pending=0;
        };
        fast_rows<7>(events.blocks[i],[&](const auto& f) {
            if (f[0].empty()) return;
            i64 ts=dates.parse(f[2]), ingested=dates.parse(f[3]);
            u64 user=0; u32 price=0,category=0;
            if (!f[1].empty() && !f[5].empty() && ts>=w.lo && ts<w.hi && ingested-ts<=day_ms) {
                if (f[4]=="view" || f[4]=="cart") user=users.get(f[1]);
                else if (f[4]=="purchase") {
                    auto amount=decimal(f[6],10'000'000);
                    if (amount>0) {
                        price=amount; user=users.get(f[1]);
                        category=catalog.category_of(f[5]);
                    }
                }
            }
            links.prefetch(user);
            batch[pending++]={event_ids.get(f[0]),user,{u64(ingested),Time(user?ts-base:0),0,price,category}};
            if (pending==batch.size()) flush();
        });
        flush();
    });
    mark("events");
    parallel(clicks.blocks.size(),threads,[&](int t,size_t i) {
        auto& worker=workers[t];
        fast_rows<4>(clicks.blocks[i],[&](const auto& f) {
            if (f[0].empty() || f[1].empty()) return;
            i64 ts=dates.parse(f[3]);
            if (ts<w.lo-week_ms || ts>=w.hi) return;
            u32 campaign=catalog.campaign(f[2]);
            if (campaign) {
                u32 person=links.person(users.get(f[1]));
                worker.timeline[person&(bins-1)].append({click_ids.get(f[0]),Time(ts-base),person,0,campaign|click_flag});
            }
        });
    });
    mark("clicks");
    parallel(bins,threads,[&](int,size_t b) {
        size_t count=0;
        for (auto& worker:workers) count+=worker.receipts[b].size();
        FlatMap<Record<Time>*> winners(count);
        for (auto& worker:workers) {
            auto& receipts=worker.receipts[b];
            for (auto& r:receipts) {
                auto& slot=winners.locate(r.id);
                if (!slot.key) slot={r.id,r.record};
                else if (i64(r.record->ingest_or_click)<i64(slot.value->ingest_or_click)) {
                    slot.value->person=0; slot.value=r.record;
                } else r.record->person=0;
            }
            std::vector<Receipt<Time>>().swap(receipts);
        }
    });
    mark("dedup");
    std::vector<Aggregator> agg;
    for (int i=0;i<threads;i++) agg.emplace_back(catalog,click_ids,w);
    parallel(bins,threads,[&](int t,size_t b) {
        std::vector<u32> start(links.extent(b)+1);
        for (auto& worker:workers) worker.timeline[b].each([&](const Record<Time>& row) {
            if (row.person) ++start[row.person/bins-1];
        });
        u32 total=0;
        for (auto& count:start) { total+=count; count=total; }
        std::unique_ptr<Moment<Time>[]> ordered(new Moment<Time>[total]);
        for (auto& worker:workers) {
            auto& pages=worker.timeline[b];
            pages.each([&](const Record<Time>& row) {
                if (!row.person) return;
                u64 value=(row.label&click_flag)?row.ingest_or_click:row.amount;
                ordered[--start[row.person/bins-1]]={value,row.ts,row.label};
            });
            pages.release();
        }
        for (size_t j=0;j+1<start.size();j++) if (start[j]<start[j+1])
            agg[t].person(links.name(b,j),ordered.get()+start[j],ordered.get()+start[j+1],base);
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
        fs::path data=argc>1?argv[1]:env("DATA_DIR","/data");
        fs::path out=argc>2?argv[2]:env("OUTPUT_DIR","/output");
        int threads=std::clamp(std::atoi(env("NCPU","8")),1,64);
        Window w=manifest(data/"manifest.json");
        if (w.hi-w.lo<=i64(UINT32_MAX)-week_ms) run<u32>(data,out,threads,w);
        else run<i64>(data,out,threads,w);
    } catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
