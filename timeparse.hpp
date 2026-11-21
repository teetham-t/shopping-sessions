#pragma once
#include "input.hpp"

inline u64 read_word(const char* p) { u64 v; memcpy(&v,p,8); return v; }
inline bool eight_digits(u64 v) {
    return !(((v+0x4646464646464646ULL)|(v-0x3030303030303030ULL))&0x8080808080808080ULL);
}
inline unsigned eight_value(u64 v) {
    v-=0x3030303030303030ULL;
    v=(v*10+(v>>8))&0x00ff00ff00ff00ffULL;
    v=(v*100+(v>>16))&0x0000ffff0000ffffULL;
    return unsigned(v*10000+(v>>32));
}

class Dates {
    struct Entry { u64 head=0; uint16_t tail=0; i64 midnight=0; };
    std::array<Entry,128> cache{};
    static unsigned slot(const char* p) {
        return ((unsigned(p[6])&3)<<5) | (((unsigned(p[8])&3)*10+(unsigned(p[9])&15))&31);
    }
public:
    Dates(i64 start,i64 end) {
        // Calendar spellings are derived from the manifest. Out-of-cache dates
        // always take the complete Gregorian parser, including distant ingests.
        i64 first=start/day_ms-9, last=std::min(end/day_ms+8,first+90);
        for (i64 d=first;d<=last;d++) {
            time_t seconds=d*86400;
            struct tm tm;
            if (!gmtime_r(&seconds,&tm)) continue;
            char label[24];
            if (strftime(label,sizeof(label),"%Y-%m-%d",&tm)!=10) continue;
            auto& e=cache[slot(label)];
            e.head=read_word(label); memcpy(&e.tail,label+8,2); e.midnight=d*day_ms;
        }
    }
    i64 parse(View s) const {
        const char* p=s.data();
        if (s.size()==13) {
            u64 a=read_word(p), b=read_word(p+5);
            if (!eight_digits(a) || !eight_digits(b)) return bad_time;
            return i64(eight_value(a))*100000 + eight_value(b)%100000;
        }
        if (s.size()!=20 && s.size()!=25) return timestamp(s);
        const auto& e=cache[slot(p)];
        uint16_t tail; memcpy(&tail,p+8,2);
        if (read_word(p)!=e.head || tail!=e.tail) return timestamp(s);
        const u64 clock=read_word(p+11);
        constexpr u64 colons=0x0000ff0000ff0000ULL;
        if (p[10]!='T' || (clock&colons)!=(0x3a3a3a3a3a3a3a3aULL&colons) ||
            !eight_digits((clock&~colons)|(0x3030303030303030ULL&colons))) return bad_time;
        auto two=[](const char* x) { return (x[0]-'0')*10+(x[1]-'0'); };
        int h=two(p+11), m=two(p+14), sec=two(p+17);
        if (h>23 || m>59 || sec>59) return bad_time;
        int offset=0;
        if (s.size()==20) {
            if (p[19]!='Z') return bad_time;
        } else {
            if ((p[19]!='+' && p[19]!='-') || p[22]!=':') return bad_time;
            int oh=pair_digits(p+20), om=pair_digits(p+23);
            if (oh>23 || om>59) return bad_time;
            offset=(oh*60+om)*(p[19]=='+'?1:-1);
        }
        return e.midnight+((h*60+m-offset)*60+sec)*1000;
    }
};
