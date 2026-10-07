# syntax=docker/dockerfile:1
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base pkgconf \
      curl-dev curl-static openssl-libs-static zlib-static zstd-static \
      brotli-static nghttp2-static libidn2-static libunistring-static libpsl-static \
      c-ares-static
WORKDIR /src
COPY src/iptv.c .
# 静态链接 libcurl 及其全部依赖，产物无任何动态库依赖；
# 构建期顺带跑 --selftest 校验 MD5/SHA-256（QEMU 下逐架构验证签名算法）
RUN gcc -O2 -s -Wall -static -o iptv iptv.c \
      $(pkg-config --static --cflags --libs libcurl) -lpthread \
    && ./iptv --selftest

FROM scratch
COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY --from=build /src/iptv /iptv
COPY channels.conf /iptv/channels.conf
USER 1000:1000
EXPOSE 1905
CMD ["/iptv"]
