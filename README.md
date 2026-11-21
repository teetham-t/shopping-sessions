# Shopping Sessions

Team **ttham.t**'s C++20 entry for the Databricks Shopping Sessions track at the Builders Buildathon.

The executable reads the manifest, catalogs and all three CSV directories, and writes the user and campaign daily tables to Parquet. The Docker image contains a statically linked executable. It runs offline as uid 1000.

## Run

From the repository root, with an input directory named `data` and a fresh writable output directory:

```sh
docker buildx build --platform linux/amd64 --load -t shopping-sessions .
mkdir -p output
chmod 777 output
docker run --rm --platform linux/amd64 --read-only --network none --user 1000:1000 \
  --cpus 8 --memory 24g --memory-swap 24g -e NCPU=8 \
  -v "$PWD/data:/data:ro" -v "$PWD/output:/output" shopping-sessions
```

On a native Linux or macOS host:

```sh
c++ -std=c++20 -O3 -DNDEBUG -pthread main.cpp -o sessions
./sessions data output
```

## Implementation

CSV readers divide uneven files at line boundaries. They validate quote placement and preserve field bytes, including spaces and carriage returns. A disjoint-set forest resolves transitive identities and selects the byte-wise smallest name in each component.

Workers partition events by their exact event ID. Each partition retains the earliest well-formed copy, including an invalid winner that suppresses later valid copies. The retained events and valid clicks are then grouped by person and sorted by time. Keeping identical repeated clicks in the time stream preserves both the minimum tied click ID and the set of distinct assisting campaigns. The session scan assigns purchases and category totals to the session's start date. A small Parquet writer emits typed dates, 64-bit totals and optional UTF-8 categories.

## Verification

Download the Polars starter, Rust generator and development answers from the [official track](https://www.joinbuilders.org/buildathon/tracks/shopping-sessions). From this repository, with the three archives extracted alongside it and Polars installed in the testing Python environment:

```sh
python3 ../starter-polars/scripts/run_fixtures.py ../starter-polars/fixtures -- ./sessions
cargo build --release --manifest-path ../generator/Cargo.toml
../generator/target/release/generate --seed 1 --events 1000000 --clicks 250000 --output ../dev
./sessions ../dev ../out-dev
python3 ../starter-polars/scripts/compare.py ../out-dev ../dev-answer
```

The public development answer contains 117,038 user-day rows and 27,400 campaign-day rows. The 36 fixtures also exercise empty outputs and exact boundary behavior. Set `SESSION_PROFILE=1` for per-stage wall times on stderr.

## License and provenance

MIT. Team ttham.t wrote the pipeline and Parquet writer for this event with AI assistance. The build uses GCC, the C++ standard library and POSIX system interfaces. The calendar arithmetic follows Howard Hinnant's Gregorian era decomposition. The writer implements the [Apache Parquet file format](https://github.com/apache/parquet-format). The official generator, fixtures and comparison tools are used separately for verification.
