# syntax=docker/dockerfile:1
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base curl-dev
WORKDIR /src
COPY src/iptv.c .
RUN gcc -O2 -s -Wall -o iptv iptv.c -lcurl -lpthread

FROM alpine:3.20
RUN apk add --no-cache libcurl ca-cert-bundle && adduser -D iptv
COPY --from=build /src/iptv /usr/local/bin/iptv
COPY channels.conf /iptv/channels.conf
USER iptv
EXPOSE 1905
HEALTHCHECK --interval=60s --timeout=5s CMD wget -q -O /dev/null http://127.0.0.1:1905/health || exit 1
CMD ["iptv"]
