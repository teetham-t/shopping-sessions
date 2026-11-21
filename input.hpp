#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__SSE2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fs = std::filesystem;
using i64 = int64_t;
using u64 = uint64_t;
using u32 = uint32_t;
using View = std::string_view;
constexpr i64 day_ms = 86'400'000, week_ms = 7 * day_ms, bad_time = INT64_MIN;

inline u64 mix(u64 value) {
    value ^= value >> 31;
    value *= 0x9e3779b185ebca87ULL;
    return value ^ (value >> 32);
}

// Store the bytes of common short identifiers directly. All other byte strings
// have a disjoint, exact interned representation, including embedded NULs.
class Keys {
    static constexpr u64 indirect = 1ULL << 63;
    char prefix;
    std::mutex lock;
    std::deque<std::string> names;
    std::unordered_map<View, u64> index;
public:
    explicit Keys(char p): prefix(p) {}
    u64 get(View field) {
        size_t n = field.size() - 1;
        if (n < 9 && n && field[0] == prefix) {
            u64 raw = 0;
            if (n==8) std::memcpy(&raw,field.data()+1,8);
            else std::memcpy(&raw,field.data()+1,n);
            u64 key = __builtin_bswap64(raw);
            // Padding can never alias strings if the final byte is nonzero.
            if (!(key & indirect) && field.back() != 0) return key;
        }
        std::lock_guard<std::mutex> guard(lock);
        auto it = index.find(field);
        if (it != index.end()) return it->second;
        u64 id = indirect + names.size();
        names.emplace_back(field);
        index.emplace(names.back(), id);
        return id;
    }
    // Called only after producers have finished interning into this dictionary.
    std::string name(u64 id) const {
        if (id & indirect) return names[id-indirect];
        char data[9] = {prefix};
        u64 raw = __builtin_bswap64(id);
        std::memcpy(data+1, &raw, 8);
        size_t n = 9;
        while (n > 1 && data[n-1] == 0) --n;
        return {data, n};
    }
    bool less(u64 a, u64 b) const {
        if (!((a|b) & indirect)) return a < b;
        return name(a) < name(b);
    }
};

inline i64 decimal(View s, i64 limit) {
    if (s.empty()) return -1;
    i64 value = 0;
    for (unsigned char ch : s) {
        unsigned d = unsigned(ch) - '0';
        if (d > 9 || value > (limit-i64(d))/10) return -1;
        value = value*10+d;
    }
    return value <= limit ? value : -1;
}

inline int pair_digits(const char* s) {
    unsigned a = (unsigned char)s[0]-unsigned('0'), b = (unsigned char)s[1]-unsigned('0');
    return (a<=9 && b<=9) ? int(a*10+b) : 100;
}

inline i64 timestamp(View s) {
    size_t n = s.size();
    if ((n != 20 && n != 25) || s[4] != '-') {
        i64 ms = decimal(s, INT64_MAX);
        return ms < 0 ? bad_time : ms;
    }
    const char *p = s.data();
    if (p[7]!='-' || p[10]!='T' || p[13]!=':' || p[16]!=':') return bad_time;
    int century=pair_digits(p), yy=pair_digits(p+2);
    int y=century*100+yy, m=pair_digits(p+5), d=pair_digits(p+8);
    int h=pair_digits(p+11), min=pair_digits(p+14), sec=pair_digits(p+17);
    if (century>99 || yy>99 || m<1 || m>12 || h>23 || min>59 || sec>59) return bad_time;
    constexpr int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = m==2 && y%4==0 && (y%100!=0 || y%400==0);
    if (d<1 || d>lengths[m-1]+leap) return bad_time;
    int offset=0;
    if (n==20) {
        if (p[19]!='Z') return bad_time;
    } else {
        if ((p[19]!='+' && p[19]!='-') || p[22]!=':') return bad_time;
        int oh=pair_digits(p+20), om=pair_digits(p+23);
        if (oh>23 || om>59) return bad_time;
        offset=(oh*60+om)*(p[19]=='+' ? 1 : -1);
    }
    // March-based Gregorian calendar, with 1970-01-01 as day zero.
    y -= m<=2;
    int era=(y>=0 ? y : y-399)/400, year=y-era*400;
    i64 days=era*146097 + year*365 + year/4-year/100 + (153*(m+(m>2?-3:9))+2)/5+d-1-719468;
    return days*day_ms + ((h*60+min-offset)*60+sec)*1000;
}

struct Mapping {
    const char *data = nullptr;
    size_t size = 0;
    explicit Mapping(const fs::path& path) {
        int fd = open(path.c_str(), O_RDONLY);
        struct stat st;
        if (fd<0 || fstat(fd,&st)!=0) throw std::runtime_error("Cannot read "+path.string());
        size = st.st_size;
        if (size) {
            data = static_cast<const char*>(mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0));
            if (data==MAP_FAILED) throw std::runtime_error("Cannot map "+path.string());
        }
        close(fd);
    }
    ~Mapping() { if (size) munmap(const_cast<char*>(data),size); }
    Mapping(const Mapping&) = delete;
};

struct Block { const char* begin; const char* end; };
struct Inputs {
    std::deque<Mapping> files;
    std::vector<Block> blocks;
    void add(const fs::path& path) {
        auto& f=files.emplace_back(path);
        if (!f.size) return;
        const char* end=f.data+f.size;
        const char* header=static_cast<const char*>(memchr(f.data,'\n',f.size));
        if (!header) return;
        const char* p=header+1;
        constexpr size_t block_size=512*1024;
        while (p<end) {
            const char* cut=end;
            if (size_t(end-p)>block_size) {
                auto nl=static_cast<const char*>(memchr(p+block_size,'\n',end-p-block_size));
                if (nl) cut=nl+1;
            }
            blocks.push_back({p,cut});
            p=cut;
        }
    }
    explicit Inputs(const fs::path& path) {
        if (fs::is_directory(path)) {
            for (auto& e:fs::directory_iterator(path))
                if (e.path().extension()==".csv" && e.is_regular_file()) add(e.path());
        } else add(path);
    }
};

// Parse one complete line. All commas are separators even if a quote is bad.
template<size_t N>
inline bool split(View row, std::array<View,N>& fields) {
    const char* p=row.data();
    const char* end=p+row.size();
    for (size_t i=0;i<N;i++) {
        const char* q=static_cast<const char*>(memchr(p,',',end-p));
        if (bool(q)!=(i+1<N)) return false;
        if (!q) q=end;
        View s(p,q-p);
        if (!s.empty() && s.front()=='"') {
            if (s.size()<2 || s.back()!='"') return false;
            s.remove_prefix(1); s.remove_suffix(1);
        }
        if (s.find('"')!=View::npos) return false;
        fields[i]=s;
        if (i+1<N) p=q+1;
    }
    return true;
}

template<size_t N, class F> void rows(Block block, F emit) {
    std::array<View,N> fields;
    const char* p=block.begin;
    while (p<block.end) {
        const char* end=static_cast<const char*>(memchr(p,'\n',block.end-p));
        if (!end) end=block.end;
        if (split(View(p,end-p),fields)) emit(fields);
        if (end==block.end) break;
        p=end+1;
    }
}

template<class F> void parallel(size_t jobs, int threads, F work) {
    if (!jobs) return;
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    auto loop=[&](int ti) {
        for (;;) {
            size_t i=next.fetch_add(1,std::memory_order_relaxed);
            if (i>=jobs) break;
            work(ti,i);
        }
    };
    for (int i=1;i<std::min<size_t>(threads,jobs);++i) pool.emplace_back(loop,i);
    loop(0);
    for (auto& t:pool) t.join();
}

inline size_t capacity(size_t count) {
    size_t result=8;
    while (result < count+count/2) result*=2;
    return result;
}

// Open addressing over exact byte keys. Zero is reserved for the empty slot.
template<class T> struct FlatMap {
    struct Slot { u64 key=0; T value{}; };
    std::vector<Slot> slots;
    size_t used=0;
    explicit FlatMap(size_t expected=0):slots(capacity(expected)) {}
    Slot& locate(u64 key) {
        size_t i=mix(key)&(slots.size()-1);
        while (slots[i].key && slots[i].key!=key) i=(i+1)&(slots.size()-1);
        return slots[i];
    }
    Slot& insert(u64 key, T value) {
        if (used*4>=slots.size()*3) {
            std::vector<Slot> old(slots.size()*2);
            old.swap(slots);
            for (auto& s:old) if (s.key) locate(s.key)=s;
        }
        auto& s=locate(key);
        if (!s.key) { s={key,value}; ++used; }
        return s;
    }
};
