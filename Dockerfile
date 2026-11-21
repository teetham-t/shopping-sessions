FROM gcc:14-bookworm AS build
WORKDIR /src
COPY *.hpp main.cpp ./
RUN g++ -std=c++20 -O3 -DNDEBUG -flto -pthread -static -s main.cpp -o /sessions

FROM scratch
COPY --from=build /sessions /sessions
USER 1000:1000
ENTRYPOINT ["/sessions"]
