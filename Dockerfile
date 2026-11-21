FROM gcc:14-bookworm@sha256:5e927c284bf55a7dc796262e311a0703344f62f41f5621eb56843111b1d37e15 AS build
WORKDIR /src
COPY *.hpp main.cpp launch.c ./
RUN mkdir /bin-out && gcc -O2 -static -s launch.c -o /bin-out/sessions && \
    g++ -std=c++20 -O3 -DNDEBUG -pthread -static -s main.cpp -o /bin-out/sessions-portable && \
    if [ "$(uname -m)" = x86_64 ]; then \
      g++ -std=c++20 -O3 -DNDEBUG -march=x86-64-v3 -pthread -static -s main.cpp -o /bin-out/sessions-v3; fi

FROM scratch
COPY --from=build /bin-out/ /
USER 1000:1000
ENTRYPOINT ["/sessions"]
