#pragma once
#include "input.hpp"

// Transpose the comparisons for 16 input bytes into a bit mask. Parsing
// operates on complete physical lines so a malformed quote cannot swallow one.
#if defined(__ARM_NEON)
inline u32 mask16(uint8x16_t v) {
    const uint8x16_t weights={1,2,4,8,16,32,64,128,1,2,4,8,16,32,64,128};
    v=vandq_u8(v,weights);
    return vaddv_u8(vget_low_u8(v)) | (u32(vaddv_u8(vget_high_u8(v)))<<8);
}
#endif
struct Marks { u64 comma, lf, quote; };
inline Marks marks64(const char* data) {
    Marks m{};
#if defined(__AVX2__)
    for (unsigned i=0;i<64;i+=32) {
        auto v=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(data+i));
        m.comma |= u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8(',')))))<<i;
        m.lf |= u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8('\n')))))<<i;
        m.quote |= u64(u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v,_mm256_set1_epi8('"')))))<<i;
    }
#elif defined(__SSE2__) || defined(__ARM_NEON)
    for (unsigned i=0;i<64;i+=16) {
#if defined(__SSE2__)
        auto v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(data+i));
        auto compare=[&](char c) { return u32(_mm_movemask_epi8(_mm_cmpeq_epi8(v,_mm_set1_epi8(c)))); };
#else
        auto v=vld1q_u8(reinterpret_cast<const uint8_t*>(data+i));
        auto compare=[&](char c) { return mask16(vceqq_u8(v,vdupq_n_u8(c))); };
#endif
        m.comma |= u64(compare(','))<<i; m.lf |= u64(compare('\n'))<<i; m.quote |= u64(compare('"'))<<i;
    }
#else
    for (unsigned i=0;i<64;++i) {
        u64 bit=u64(1)<<i;
        if (data[i]==',') m.comma|=bit;
        if (data[i]=='\n') m.lf|=bit;
        if (data[i]=='"') m.quote|=bit;
    }
#endif
    return m;
}

#if defined(__AVX2__) && defined(__BMI2__)
// Gather quote bits at field starts and ends with PEXT. Matched pairs and
// adjacency validate the complete row before any selected field is read.
template<size_t N,class F> inline void marked_row(const char* p,Marks a,Marks b,unsigned length,F& emit) {
    unsigned width=length&63;
    u64 through=~u64(0)>>(63-width);
    if (length<64) { a.comma&=through; a.quote&=through; b={}; }
    else { b.comma&=through; b.quote&=through; }
    u64 lo=a.comma, hi=b.comma;
    if (length<64) lo|=u64(1)<<width; else hi|=u64(1)<<width;
    unsigned low_count=__builtin_popcountll(lo);
    if (low_count+__builtin_popcountll(hi)!=N) return;
    u64 left_lo=(lo<<1)|1, left_hi=(hi<<1)|(lo>>63);
    u64 right_lo=(lo>>1)|(hi<<63), right_hi=hi>>1;
    if ((a.quote&~(left_lo^right_lo)) || (b.quote&~(left_hi^right_hi))) return;
    u64 after=_pext_u64((a.quote>>1)|(b.quote<<63),lo) | (_pext_u64(b.quote>>1,hi)<<low_count);
    u64 open=((after<<1)|(a.quote&1))&((u64(1)<<N)-1);
    u64 close=_pext_u64(a.quote<<1,lo) | (_pext_u64((b.quote<<1)|(a.quote>>63),hi)<<low_count);
    if (open!=close) return;
    std::array<unsigned,N> ends;
    unsigned j=0;
    while (lo) { ends[j++]=__builtin_ctzll(lo); lo&=lo-1; }
    while (hi) { ends[j++]=64+__builtin_ctzll(hi); hi&=hi-1; }
    std::array<View,N> fields;
    unsigned start=0;
    for (size_t k=0;k<N;k++) {
        unsigned q=(open>>k)&1;
        fields[k]=View(p+start+q,ends[k]-start-2*q);
        start=ends[k]+1;
    }
    emit(fields);
}
#endif

template<size_t N, class F> void fast_rows(Block block, F emit) {
#if defined(__AVX2__) && defined(__BMI2__)
    const char* at=block.begin;
    while (block.end-at>=128) {
        Marks a=marks64(at), b{};
        unsigned n;
        if (a.lf) n=__builtin_ctzll(a.lf);
        else {
            b=marks64(at+64);
            if (!b.lf) {
                const char* lf=static_cast<const char*>(memchr(at+128,'\n',block.end-at-128));
                const char* stop=lf?lf+1:block.end;
                rows<N>({at,stop},emit); at=stop; continue;
            }
            n=64+__builtin_ctzll(b.lf);
        }
        marked_row<N>(at,a,b,n,emit); at+=n+1;
    }
    rows<N>({at,block.end},emit);
    return;
#endif
    // Offsets in each tile fit in 15 bits. High bits mark parity of the
    // cumulative quote count. It must agree at both ends of every field.
    constexpr int tile=8192;
    std::array<uint16_t,tile+8> separators, lines, broken;
    std::array<View,N> fields;
    const char* base=block.begin;
    while (block.end-base>=256) {
        const char* end=base+std::min<ptrdiff_t>(tile,block.end-base);
        while (end>base && end[-1]!='\n') --end;
        if (end-base<128) break;
        int nc=0,nl=0,nb=0;
        int parity=0, pos=0;
        // Include an LF at both tile boundaries when checking quote neighbors.
        auto scan=[&](Marks m,u64 previous_sep,u64 next_sep,int remaining) {
            u64 del=m.comma|m.lf;
            u64 neighbors=(del<<1)|previous_sep;
            neighbors|=(del>>1)|(next_sep<<63);
            u64 illegal=m.quote & ~neighbors;
            // Prefix XOR gives the quote parity at each byte. Isolated quotes
            // next to just one delimiter are caught by the parity difference.
            u64 prefix=m.quote; prefix^=prefix<<1; prefix^=prefix<<2; prefix^=prefix<<4;
            prefix^=prefix<<8; prefix^=prefix<<16; prefix^=prefix<<32;
            prefix^=u64(0)-u64(parity);
            parity=(prefix>>(remaining-1))&1;
            while (del) {
                unsigned at=__builtin_ctzll(del); del&=del-1;
                separators[nc++]=uint16_t(pos+at)|uint16_t(((prefix>>at)&1)<<15);
            }
            while (m.lf) {
                unsigned at=__builtin_ctzll(m.lf); m.lf&=m.lf-1;
                lines[nl++]=pos+at;
            }
            while (illegal) {
                unsigned at=__builtin_ctzll(illegal); illegal&=illegal-1;
                broken[nb++]=pos+at;
            }
        };
        while (end-base-pos>=64) {
            auto m=marks64(base+pos);
            bool prev=pos==0 || base[pos-1]==',' || base[pos-1]=='\n';
            bool next=base+pos+64==end || base[pos+64]==',' || base[pos+64]=='\n';
            scan(m,prev,next,64); pos+=64;
        }
        if (base+pos<end) {
            char tail[64]={}; int n=end-base-pos;
            memcpy(tail,base+pos,n);
            auto m=marks64(tail);
            bool prev=pos==0 || base[pos-1]==',' || base[pos-1]=='\n';
            scan(m,prev,0,n);
        }
        broken[nb]=UINT16_MAX;
        int token=0, begin=0, bad=0;
        uint16_t before=0;
        for (int line=0;line<nl;++line) {
            int lf=lines[line];
            bool shape=token+int(N)<=nc && (separators[token+N-1]&32767)==lf;
            bool valid=shape && broken[bad]>=lf;
            if (valid) {
                unsigned parity_diff=0;
                for (size_t j=0;j<N;j++) {
                    unsigned word=separators[token+j], finish=word&32767;
                    parity_diff|=word^before;
                    int quote=base[begin]=='"';
                    auto length=int(finish)-begin-2*quote;
                    fields[j]=View(base+begin+quote,std::max(length,0));
                    begin=finish+1;
                }
                if (!(parity_diff&32768)) emit(fields);
            }
            if (shape) token+=N;
            else { while ((separators[token++]&32767)!=lf) {} }
            before=separators[token-1]&32768;
            begin=lf+1;
            while (broken[bad]<lf) ++bad;
        }
        base=end;
    }
    rows<N>({base,block.end},emit);
}
