#pragma once
#include "input.hpp"

// Flat, little-endian Parquet. Each row group uses one uncompressed PLAIN
// data page per column. The single nullable column uses bit-packed def levels.
namespace parquet {
enum CompactType { I32=5, I64=6, Binary=8, List=9, Struct=12 };
struct Compact {
    std::string bytes;
    int last=0;
    void var(u64 n) {
        while (n>=128) { bytes.push_back(char((n&127)|128)); n>>=7; }
        bytes.push_back(char(n));
    }
    void number(i64 n) { var((u64(n)<<1)^u64(-(n<0))); }
    void field(int id, CompactType t) {
        bytes.push_back(char(((id-last)<<4)|t)); last=id;
    }
    void integer(int id, i64 n, CompactType t=I32) { field(id,t); number(n); }
    void str(View s) { var(s.size()); bytes.append(s); }
    void string_field(int id, View s) { field(id,Binary); str(s); }
    void list(int id, size_t n, CompactType t) {
        field(id,List);
        bytes.push_back(char((std::min<size_t>(15,n)<<4)|t));
        if (n>=15) var(n);
    }
    template<class Fn> void object(Fn fn) {
        int parent=last; last=0; fn(); bytes.push_back(0); last=parent;
    }
};
struct Column {
    const char* name;
    int physical;
    int converted=-1;
    bool optional=false;
};
struct Values {
    std::string bytes, valid;
    template<class T> void put(T v) { bytes.append(reinterpret_cast<const char*>(&v),sizeof(v)); }
    void text(View s) { put(int32_t(s.size())); bytes.append(s); }
    void defined(size_t row, bool present) {
        if ((row&7)==0) valid.push_back(0);
        if (present) valid.back() |= char(1u<<(row&7));
    }
};
class File {
    struct Chunk { i64 offset,size; };
    struct Group { int rows; i64 size=0; std::vector<Chunk> chunks; };
    std::ofstream out;
    std::vector<Column> schema;
    std::vector<Group> groups;
    i64 total=0;
    void write(View s) { out.write(s.data(),s.size()); }
public:
    File(const fs::path& path,std::vector<Column> columns):schema(std::move(columns)) {
        out.exceptions(std::ios::failbit|std::ios::badbit);
        out.open(path,std::ios::binary); write("PAR1");
    }
    void append(int n, std::vector<Values>& values) {
        if (!n) return;
        Group group{n,0,{}};
        for (size_t i=0;i<schema.size();i++) {
            auto& col=values[i];
            Compact levels;
            if (schema[i].optional) {
                levels.var((col.valid.size()<<1)|1);
                levels.bytes.append(col.valid);
                int32_t length=levels.bytes.size();
                levels.bytes.insert(0,reinterpret_cast<const char*>(&length),4);
            }
            i64 size=levels.bytes.size()+col.bytes.size();
            Compact h;
            h.object([&] {
                h.integer(1,0); h.integer(2,size); h.integer(3,size);
                h.field(5,Struct); h.object([&] {
                    h.integer(1,n); h.integer(2,0); h.integer(3,3); h.integer(4,3);
                });
            });
            i64 offset=out.tellp(); size+=h.bytes.size();
            group.chunks.push_back({offset,size}); group.size+=size;
            write(h.bytes); write(levels.bytes); write(col.bytes);
            col.bytes.clear(); col.valid.clear();
        }
        groups.push_back(std::move(group)); total+=n;
    }
    void finish() {
        Compact f;
        f.object([&] {
            f.integer(1,1);
            f.list(2,schema.size()+1,Struct);
            f.object([&] { f.string_field(4,"schema"); f.integer(5,schema.size()); });
            for (const auto& c:schema) f.object([&] {
                f.integer(1,c.physical); f.integer(3,c.optional ? 1:0); f.string_field(4,c.name);
                if (c.converted>=0) f.integer(6,c.converted);
            });
            f.integer(3,total,I64);
            f.list(4,groups.size(),Struct);
            for (auto& g:groups) f.object([&] {
                f.list(1,schema.size(),Struct);
                for (size_t i=0;i<schema.size();i++) f.object([&] {
                    auto c=g.chunks[i];
                    f.integer(2,c.offset,I64);
                    f.field(3,Struct); f.object([&] {
                        f.integer(1,schema[i].physical);
                        f.list(2,2,I32); f.number(0); f.number(3);
                        f.list(3,1,Binary); f.str(schema[i].name);
                        f.integer(4,0); f.integer(5,g.rows,I64);
                        f.integer(6,c.size,I64); f.integer(7,c.size,I64); f.integer(9,c.offset,I64);
                    });
                });
                f.integer(2,g.size,I64); f.integer(3,g.rows,I64);
            });
        });
        write(f.bytes);
        int32_t size=f.bytes.size();
        out.write(reinterpret_cast<const char*>(&size),4);
        write("PAR1"); out.close();
    }
};
}
