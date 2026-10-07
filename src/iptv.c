/*
 * iptv-c —— 极简 IPTV 直播源分发服务（单文件 C 实现）
 *
 * 功能（移植自 akiralereal/iptv 的核心取流逻辑，砍掉管理台/Chromium/用户系统）：
 *   GET /interface.m3u   输出播放列表（频道来自 channels.conf，热加载）
 *   GET /migu/<pID>      咪咕 720p 取流 -> 302 重定向（签名算法移植 getAndroidURL720p）
 *   GET /hntv/<cid>      河南大象新闻取流 -> 302 重定向（SHA-256 请求签名）
 *   GET /hbtv/<id>.m3u8  湖北长江云 HLS 全代理（清单改写 + 分片转发，带官网 Referer）
 *   GET /seg/<key>.<ext> hbtv 分片/子清单代理
 *   GET /health          健康检查
 *
 * 依赖: libcurl + pthread。内存占用约 5MB 量级。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <curl/curl.h>

/* ================= MD5（RFC 1321） ================= */
typedef struct { unsigned int a, b, c, d; unsigned long long len; unsigned char buf[64]; size_t buflen; } MD5_CTX;

static unsigned int md5_leftrotate(unsigned int x, unsigned int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(MD5_CTX *ctx, const unsigned char *p) {
    static const unsigned int K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const unsigned char S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    unsigned int w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (unsigned int)p[i*4] | ((unsigned int)p[i*4+1] << 8) | ((unsigned int)p[i*4+2] << 16) | ((unsigned int)p[i*4+3] << 24);
    unsigned int a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
    for (int i = 0; i < 64; i++) {
        unsigned int f; int g;
        if (i < 16)      { f = (b & c) | (~b & d);       g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);       g = (5*i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;                g = (3*i + 5) % 16; }
        else             { f = c ^ (b | ~d);             g = (7*i) % 16; }
        unsigned int tmp = d;
        d = c; c = b;
        b = b + md5_leftrotate(a + f + K[i] + w[g], S[i]);
        a = tmp;
    }
    ctx->a += a; ctx->b += b; ctx->c += c; ctx->d += d;
}

static void md5_init(MD5_CTX *ctx) {
    ctx->a = 0x67452301; ctx->b = 0xefcdab89; ctx->c = 0x98badcfe; ctx->d = 0x10325476;
    ctx->len = 0; ctx->buflen = 0;
}

static void md5_update(MD5_CTX *ctx, const void *data, size_t len) {
    const unsigned char *p = data;
    ctx->len += len;
    while (len > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take; p += take; len -= take;
        if (ctx->buflen == 64) { md5_block(ctx, ctx->buf); ctx->buflen = 0; }
    }
}

static void md5_final(MD5_CTX *ctx, unsigned char out[16]) {
    unsigned long long bitlen = ctx->len * 8;
    unsigned char pad = 0x80;
    md5_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->buflen != 56) md5_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bitlen >> (8*i));
    md5_update(ctx, lenb, 8);
    unsigned int v[4] = { ctx->a, ctx->b, ctx->c, ctx->d };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i*4+j] = (unsigned char)(v[i] >> (8*j));
}

static void md5_hex(const char *in, char out[33]) {
    MD5_CTX ctx; unsigned char d[16];
    md5_init(&ctx); md5_update(&ctx, in, strlen(in)); md5_final(&ctx, d);
    for (int i = 0; i < 16; i++) sprintf(out + i*2, "%02x", d[i]);
    out[32] = 0;
}

/* ================= SHA-256（FIPS 180-4） ================= */
typedef struct { unsigned int h[8]; unsigned long long len; unsigned char buf[64]; size_t buflen; } SHA256_CTX;

static unsigned int rotr(unsigned int x, unsigned int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(SHA256_CTX *ctx, const unsigned char *p) {
    static const unsigned int K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i*4] << 24) | ((unsigned int)p[i*4+1] << 16) | ((unsigned int)p[i*4+2] << 8) | (unsigned int)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned int s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=ctx->h[0],b=ctx->h[1],c=ctx->h[2],d=ctx->h[3],e=ctx->h[4],f=ctx->h[5],g=ctx->h[6],h=ctx->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[i] + w[i];
        unsigned int S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d; ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

static void sha256_init(SHA256_CTX *ctx) {
    static const unsigned int H0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(ctx->h, H0, sizeof H0);
    ctx->len = 0; ctx->buflen = 0;
}

static void sha256_update(SHA256_CTX *ctx, const void *data, size_t len) {
    const unsigned char *p = data;
    ctx->len += len;
    while (len > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take; p += take; len -= take;
        if (ctx->buflen == 64) { sha256_block(ctx, ctx->buf); ctx->buflen = 0; }
    }
}

static void sha256_final(SHA256_CTX *ctx, unsigned char out[32]) {
    unsigned long long bitlen = ctx->len * 8;
    unsigned char pad = 0x80;
    sha256_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx->buflen != 56) sha256_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[7-i] = (unsigned char)(bitlen >> (8*i));
    sha256_update(ctx, lenb, 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            out[i*4+j] = (unsigned char)(ctx->h[i] >> (24 - 8*j));
}

static void sha256_hex(const char *in, char out[65]) {
    SHA256_CTX ctx; unsigned char d[32];
    sha256_init(&ctx); sha256_update(&ctx, in, strlen(in)); sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) sprintf(out + i*2, "%02x", d[i]);
    out[64] = 0;
}

/* ================= 配置 ================= */
static int  g_port = 1905;
static char g_epg_url[512] = "";
static int  g_rate_type = 3;          /* 咪咕画质: 2=540p 3=720p 4=1080p(需会员) */
static int  g_enable_h265 = 1;
static int  g_enable_hdr = 1;
static long g_migu_cache_ttl = 10800; /* 秒 */
static char g_channels_file[512] = "/iptv/channels.conf";

/* ================= 频道表（热加载） ================= */
#define MAX_CHANNELS 512
typedef struct {
    char type[8];            /* migu / hntv / hbtv / direct */
    char id[64];
    char name[128];
    char group[64];
    char logo[640];
    char url[2048];          /* direct 用 */
} channel_t;

static channel_t g_channels[MAX_CHANNELS];
static int g_nchannels = 0;
static time_t g_channels_mtime = 0;
static pthread_mutex_t g_channels_lock = PTHREAD_MUTEX_INITIALIZER;

static void load_channels_locked(void) {
    struct stat st;
    if (stat(g_channels_file, &st) != 0) return;
    if (st.st_mtime == g_channels_mtime && g_nchannels > 0) return;
    FILE *fp = fopen(g_channels_file, "r");
    if (!fp) return;
    char line[4096];
    int n = 0;
    while (n < MAX_CHANNELS && fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        line[strcspn(line, "\r\n")] = 0;
        channel_t *c = &g_channels[n];
        memset(c, 0, sizeof *c);
        char *fields[6] = {0};
        char *p = line;
        for (int i = 0; i < 6 && p; i++) {
            fields[i] = p;
            char *bar = strchr(p, '|');
            if (bar && i < 5) { *bar = 0; p = bar + 1; } else { p = NULL; if (i < 5) fields[i+1] = NULL; }
        }
        if (!fields[0] || !fields[1] || !fields[2]) continue;
        snprintf(c->type, sizeof c->type, "%s", fields[0]);
        snprintf(c->id, sizeof c->id, "%s", fields[1]);
        snprintf(c->name, sizeof c->name, "%s", fields[2]);
        snprintf(c->group, sizeof c->group, "%s", fields[3] ? fields[3] : "");
        snprintf(c->logo, sizeof c->logo, "%s", fields[4] ? fields[4] : "");
        snprintf(c->url, sizeof c->url, "%s", fields[5] ? fields[5] : "");
        if (strcmp(c->type, "migu") && strcmp(c->type, "hntv") &&
            strcmp(c->type, "hbtv") && strcmp(c->type, "direct")) continue;
        if (!strcmp(c->type, "direct") && !c->url[0]) continue;
        n++;
    }
    fclose(fp);
    g_nchannels = n;
    g_channels_mtime = st.st_mtime;
    fprintf(stderr, "[iptv] 已加载 %d 个频道 (%s)\n", n, g_channels_file);
}

static void maybe_reload_channels(void) {
    pthread_mutex_lock(&g_channels_lock);
    load_channels_locked();
    pthread_mutex_unlock(&g_channels_lock);
}

/* ================= 频道可用状态（独立于 conf 热加载，按 type+id 记忆） ================= */
#define CH_UNKNOWN 0
#define CH_OK      1
#define CH_BAD     2
typedef struct { char type[8]; char id[64]; int status; time_t ts; } ch_status_t;
static ch_status_t g_status[MAX_CHANNELS];
static int g_nstatus = 0;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;

static int status_get(const char *type, const char *id) {
    int st = CH_UNKNOWN;
    pthread_mutex_lock(&g_status_lock);
    for (int i = 0; i < g_nstatus; i++)
        if (!strcmp(g_status[i].type, type) && !strcmp(g_status[i].id, id)) { st = g_status[i].status; break; }
    pthread_mutex_unlock(&g_status_lock);
    return st;
}

static void status_set(const char *type, const char *id, int st) {
    pthread_mutex_lock(&g_status_lock);
    int slot = -1;
    for (int i = 0; i < g_nstatus; i++)
        if (!strcmp(g_status[i].type, type) && !strcmp(g_status[i].id, id)) { slot = i; break; }
    if (slot < 0 && g_nstatus < MAX_CHANNELS) slot = g_nstatus++;
    if (slot >= 0) {
        snprintf(g_status[slot].type, sizeof g_status[slot].type, "%s", type);
        snprintf(g_status[slot].id, sizeof g_status[slot].id, "%s", id);
        g_status[slot].status = st;
        g_status[slot].ts = time(NULL);
    }
    pthread_mutex_unlock(&g_status_lock);
}

/* ================= HTTP 工具 ================= */
static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) { if (errno == EINTR) continue; return -1; }
        p += n; left -= (size_t)n;
    }
    return (ssize_t)len;
}

static void http_respond(int fd, int code, const char *status, const char *ctype, const char *body) {
    char hdr[1024];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, status, ctype, blen);
    send_all(fd, hdr, (size_t)n);
    if (blen) send_all(fd, body, blen);
}

static void http_redirect(int fd, const char *location) {
    char hdr[4608];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", location);
    send_all(fd, hdr, (size_t)n);
}

/* ================= libcurl 抓取缓冲 ================= */
typedef struct { char *data; size_t len; size_t cap; } buf_t;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *ud) {
    buf_t *b = ud;
    size_t add = size * nmemb;
    if (b->len + add + 1 > b->cap) {
        size_t ncap = (b->cap ? b->cap : 65536);
        while (b->len + add + 1 > ncap) ncap *= 2;
        if (ncap > 8 * 1024 * 1024) return 0; /* 上限 8MB */
        char *nd = realloc(b->data, ncap);
        if (!nd) return 0;
        b->data = nd; b->cap = ncap;
    }
    memcpy(b->data + b->len, ptr, add);
    b->len += add;
    b->data[b->len] = 0;
    return add;
}

#define CA_BUNDLE_PATH "/etc/ssl/certs/ca-certificates.crt"

/* 抓取整个响应体；headers 为附加请求头。成功返回 0，body 归调用方 free。
   tag 用于日志区分调用方；所有失败都打到 stderr（docker logs 可见）。 */
static int http_fetch(const char *tag, const char *url, struct curl_slist *headers, long timeout, buf_t *body) {
    CURL *c = curl_easy_init();
    if (!c) return -1;
    memset(body, 0, sizeof *body);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "iptv-c/1.0");
    if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[iptv] %s 请求失败: curl=%d %s (%.80s)\n",
                tag, (int)rc, curl_easy_strerror(rc), url);
        free(body->data); body->data = NULL; return -2;
    }
    if (!body->data) { body->data = calloc(1, 1); body->len = 0; }
    if (code >= 400)
        fprintf(stderr, "[iptv] %s 上游返回 HTTP %ld: %.160s\n", tag, code, body->data);
    return (int)code;
}

/* 取重定向 Location（GET、不跟随、丢弃 body）。返回 1 且 loc 有效。 */
typedef struct { char location[4096]; } hdr_ctx_t;

static size_t hdr_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    hdr_ctx_t *h = ud;
    if (n > 10 && strncasecmp(ptr, "Location:", 9) == 0) {
        char *v = ptr + 9;
        while (*v == ' ' || *v == '\t') v++;
        size_t l = strcspn(v, "\r\n");
        if (l >= sizeof h->location) l = sizeof h->location - 1;
        memcpy(h->location, v, l);
        h->location[l] = 0;
    }
    return n;
}

static size_t discard_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    (void)ptr; (void)ud;
    return size * nmemb;
}

static int fetch_location(const char *url, long timeout, char *loc, size_t locsz) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    hdr_ctx_t h;
    h.location[0] = 0;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &h);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "iptv-c/1.0");
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || !h.location[0]) {
        fprintf(stderr, "[iptv] 302跟随失败: curl=%d %s (%.80s)\n",
                (int)rc, curl_easy_strerror(rc), url);
        return 0;
    }
    snprintf(loc, locsz, "%s", h.location);
    return 1;
}

/* JSON 文本中提取 "key":"value"（从 from 位置开始找），处理 \" 与 \/ 转义。 */
static int json_get_str(const char *js, const char *from, const char *key, const char *end_before,
                        char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from ? from : js, pat);
    if (!p) return 0;
    if (end_before && p >= end_before) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && n + 1 < outsz) {
        if (*p == '\\' && p[1]) {
            if (p[1] == '"' || p[1] == '/' || p[1] == '\\') { out[n++] = p[1]; p += 2; continue; }
            out[n++] = *p++; continue;
        }
        if (*p == '"') break;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

/* JSON 文本中提取数组字段的第一个字符串元素："key":["value", ...]（从 from 位置开始找） */
static int json_get_arr_first(const char *js, const char *from, const char *key, const char *end_before,
                              char *out, size_t outsz) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(from ? from : js, pat);
    if (!p) return 0;
    if (end_before && p >= end_before) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '[') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0; /* 空数组或首元素不是字符串 */
    p++;
    size_t n = 0;
    while (*p && n + 1 < outsz) {
        if (*p == '\\' && p[1]) {
            if (p[1] == '"' || p[1] == '/' || p[1] == '\\') { out[n++] = p[1]; p += 2; continue; }
            out[n++] = *p++; continue;
        }
        if (*p == '"') break;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0;
}

/* ================= 咪咕（移植 getAndroidURL720p + getddCalcuURL720p） ================= */
typedef struct { char pid[32]; char url[4096]; time_t ts; } migu_cache_t;
#define MIGU_CACHE_N 128
static migu_cache_t g_migu_cache[MIGU_CACHE_N];
static pthread_mutex_t g_migu_lock = PTHREAD_MUTEX_INITIALIZER;

static void dd_calcu_720p(const char *puData, const char *programId, char *out, size_t outsz) {
    static const char keys[] = "cdabyzwxkl";
    char datestr[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(datestr, sizeof datestr, "%04d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    size_t L = strlen(puData), n = 0;
    for (size_t i = 0; i < L / 2 && n + 4 < outsz; i++) {
        out[n++] = puData[L - i - 1];
        out[n++] = puData[i];
        char extra = 0;
        if (i == 1) extra = 'v';
        else if (i == 2) extra = keys[datestr[2] - '0'];
        else if (i == 3 && strlen(programId) > 6) extra = keys[programId[6] - '0'];
        else if (i == 4) extra = 'a';
        if (extra) out[n++] = extra;
    }
    out[n] = 0;
}

/* 缓存写入：url 为 NULL 时记「失败负缓存」（60 秒内同 pid 直接失败，不再打上游，
   防止播放器重试把咪咕风控打出来——原版同样有 1 分钟失败缓存） */
static void migu_cache_store(const char *pid, const char *url) {
    pthread_mutex_lock(&g_migu_lock);
    int slot = 0;
    time_t oldest = g_migu_cache[0].ts;
    for (int i = 0; i < MIGU_CACHE_N; i++) {
        if (!g_migu_cache[i].pid[0] || !strcmp(g_migu_cache[i].pid, pid)) { slot = i; break; }
        if (g_migu_cache[i].ts < oldest) { oldest = g_migu_cache[i].ts; slot = i; }
    }
    snprintf(g_migu_cache[slot].pid, sizeof g_migu_cache[slot].pid, "%s", pid);
    if (url) snprintf(g_migu_cache[slot].url, sizeof g_migu_cache[slot].url, "%s", url);
    else g_migu_cache[slot].url[0] = 0;
    g_migu_cache[slot].ts = time(NULL);
    pthread_mutex_unlock(&g_migu_lock);
}

/* 返回 0 成功，final 为可直接播放的 m3u8 地址 */
static int migu_resolve(const char *pid, char *final, size_t finalsz) {
    pthread_mutex_lock(&g_migu_lock);
    for (int i = 0; i < MIGU_CACHE_N; i++) {
        if (g_migu_cache[i].pid[0] && !strcmp(g_migu_cache[i].pid, pid)) {
            time_t age = time(NULL) - g_migu_cache[i].ts;
            if (g_migu_cache[i].url[0] && age < g_migu_cache_ttl) {
                snprintf(final, finalsz, "%s", g_migu_cache[i].url);
                pthread_mutex_unlock(&g_migu_lock);
                return 0;
            }
            if (!g_migu_cache[i].url[0] && age < 60) { /* 失败负缓存 */
                pthread_mutex_unlock(&g_migu_lock);
                return -4;
            }
        }
    }
    pthread_mutex_unlock(&g_migu_lock);

    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)time(NULL) * 1000);
    const char *appVer = "2600034600";

    char str1[128];
    snprintf(str1, sizeof str1, "%s%s%.8s", ts, pid, appVer);
    char md5_1[33];
    md5_hex(str1, md5_1);

    char salt[16];
    snprintf(salt, sizeof salt, "%06d25", rand() % 1000000);
    char str2[128];
    snprintf(str2, sizeof str2, "%s%s%.4s", md5_1, "2cac4f2c6c3346a5b34e085725ef7e33migu", salt);
    char sign[33];
    md5_hex(str2, sign);

    char req[1024];
    int n = snprintf(req, sizeof req,
        "https://play.miguvideo.com/playurl/v1/play/playurl?sign=%s&rateType=%d&contId=%s"
        "&timestamp=%s&salt=%s&flvEnable=true&super4k=true",
        sign, g_rate_type, pid, ts, salt);
    if (g_enable_h265) n += snprintf(req + n, sizeof req - (size_t)n, "&h265N=true");
    if (g_enable_hdr) n += snprintf(req + n, sizeof req - (size_t)n, "&4kvivid=true&2Kvivid=true&vivid=2");
    (void)n;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "AppVersion: 2600034600");
    hdrs = curl_slist_append(hdrs, "TerminalId: android");
    hdrs = curl_slist_append(hdrs, "X-UP-CLIENT-CHANNEL-ID: 2600034600-99000-201600010010028");
    /* cctv5 / cctv5+ 带 appCode 无法回放，与原逻辑一致跳过 */
    if (strcmp(pid, "641886683") && strcmp(pid, "641886773"))
        hdrs = curl_slist_append(hdrs, "appCode: miguvideo_default_android");

    buf_t body;
    int code = http_fetch("migu", req, hdrs, 10, &body);
    curl_slist_free_all(hdrs);
    if (code < 0 || !body.data) { migu_cache_store(pid, NULL); return -1; }

    char raw_url[3072];
    const char *urlInfo = strstr(body.data, "\"urlInfo\"");
    int ok = 0;
    if (urlInfo && json_get_str(body.data, urlInfo, "url", NULL, raw_url, sizeof raw_url)) {
        const char *pd = strstr(raw_url, "&puData=");
        if (!pd) pd = strstr(raw_url, "?puData=");
        if (pd) {
            pd += 8;
            size_t pdl = strlen(pd);
            char *dd = malloc(pdl + 16);
            if (dd) {
                dd_calcu_720p(pd, pid, dd, pdl + 16);
                snprintf(final, finalsz, "%s&ddCalcu=%s&sv=10004&ct=android", raw_url, dd);
                free(dd);
                ok = 1;
            }
        }
    }
    if (!ok) {
        fprintf(stderr, "[iptv] migu 响应无法解析(code=%d): %.160s\n", code, body.data);
        free(body.data);
        migu_cache_store(pid, NULL);
        return -2;
    }
    free(body.data);

    /* 跟随 302 拿到最终 CDN 地址；bofang 开头的重试（与原 get302URL 一致） */
    char loc[4096];
    char cur[4096];
    snprintf(cur, sizeof cur, "%s", final);
    int got = 0;
    for (int z = 0; z < 6; z++) {
        if (fetch_location(cur, 6, loc, sizeof loc)) {
            if (strncmp(loc, "http://bofang", 13) != 0) {
                snprintf(final, finalsz, "%s", loc);
                got = 1;
                break;
            }
        }
        usleep(150 * 1000);
    }
    if (!got) return -3;

    pthread_mutex_lock(&g_migu_lock);
    int slot = 0;
    time_t oldest = g_migu_cache[0].ts;
    for (int i = 0; i < MIGU_CACHE_N; i++) {
        if (!g_migu_cache[i].pid[0] || !strcmp(g_migu_cache[i].pid, pid)) { slot = i; break; }
        if (g_migu_cache[i].ts < oldest) { oldest = g_migu_cache[i].ts; slot = i; }
    }
    snprintf(g_migu_cache[slot].pid, sizeof g_migu_cache[slot].pid, "%s", pid);
    snprintf(g_migu_cache[slot].url, sizeof g_migu_cache[slot].url, "%s", final);
    g_migu_cache[slot].ts = time(NULL);
    pthread_mutex_unlock(&g_migu_lock);
    return 0;
}

/* ================= 河南（大象新闻，SHA-256 签名） ================= */
#define HNTV_LIST_URL "https://pubmod.hntv.tv/program/getAuth/live/class/program/11/"
#define HNTV_SIGN_SECRET "6ca114a836ac7d73"
static buf_t g_hntv_cache = {0};
static time_t g_hntv_cache_ts = 0;
static pthread_mutex_t g_hntv_lock = PTHREAD_MUTEX_INITIALIZER;

static int hntv_fetch_list_locked(buf_t *body) {
    if (g_hntv_cache.data && time(NULL) - g_hntv_cache_ts < 7200) {
        *body = g_hntv_cache;
        return 200;
    }
    char ts[16], sign[65], signdata[64];
    snprintf(ts, sizeof ts, "%lld", (long long)time(NULL));
    snprintf(signdata, sizeof signdata, "%s%s", HNTV_SIGN_SECRET, ts);
    sha256_hex(signdata, sign);

    char h1[128], h2[32];
    snprintf(h1, sizeof h1, "sign: %s", sign);
    snprintf(h2, sizeof h2, "timestamp: %s", ts);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, h1);
    hdrs = curl_slist_append(hdrs, h2);
    hdrs = curl_slist_append(hdrs, "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    hdrs = curl_slist_append(hdrs, "Origin: https://static.hntv.tv");
    hdrs = curl_slist_append(hdrs, "Referer: https://static.hntv.tv/kds/");
    hdrs = curl_slist_append(hdrs, "Accept: application/json, text/plain, */*");

    buf_t fresh;
    int code = http_fetch("hntv", HNTV_LIST_URL, hdrs, 10, &fresh);
    curl_slist_free_all(hdrs);
    if (code == 200 && fresh.data && fresh.len > 2) {
        free(g_hntv_cache.data);
        g_hntv_cache = fresh;
        g_hntv_cache_ts = time(NULL);
        *body = g_hntv_cache;
        return 200;
    }
    free(fresh.data);
    if (g_hntv_cache.data) { *body = g_hntv_cache; return 200; }
    return code;
}

static int hntv_resolve(const char *cid, char *final, size_t finalsz) {
    pthread_mutex_lock(&g_hntv_lock);
    buf_t body;
    int code = hntv_fetch_list_locked(&body);
    if (code != 200 || !body.data) {
        pthread_mutex_unlock(&g_hntv_lock);
        return -1;
    }
    /* 找 "cid":145 或 "cid":"145" */
    char pat[32];
    snprintf(pat, sizeof pat, "\"cid\":%s", cid);
    char pat2[32];
    snprintf(pat2, sizeof pat2, "\"cid\":\"%s\"", cid);
    const char *p = body.data;
    const char *hit = NULL;
    while ((p = strstr(p, "\"cid\""))) {
        if (strncmp(p, pat, strlen(pat)) == 0 || strncmp(p, pat2, strlen(pat2)) == 0) { hit = p; break; }
        p += 5;
    }
    int ok = 0;
    if (hit) {
        const char *next = strstr(hit + 5, "\"cid\"");
        char url[2048];
        if (json_get_str(body.data, hit, "url", next, url, sizeof url) ||
            json_get_arr_first(body.data, hit, "video_streams", next, url, sizeof url) ||
            json_get_arr_first(body.data, hit, "streams", next, url, sizeof url)) {
            if (strncmp(url, "http://", 7) == 0) { /* 升级到 https（与原逻辑一致） */
                snprintf(final, finalsz, "https://%s", url + 7);
            } else {
                snprintf(final, finalsz, "%s", url);
            }
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_hntv_lock);
    return ok ? 0 : -2;
}

/* ================= 湖北（长江云 HLS 全代理） ================= */
#define HBTV_PAGE_URL "https://news.hbtv.com.cn/app/tv/431"
#define HBTV_REFERER "https://news.hbtv.com.cn/"
#define HBTV_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"

typedef struct { const char *id; const char *rawName; const char *streamPath; } hbtv_def_t;
static const hbtv_def_t HBTV_CHANNELS[] = {
    { "431", "湖北卫视", "new-hbws" },
    { "432", "湖北经视", "new-hbjs" },
    { "433", "湖北综合", "new-hbzh" },
    { "435", "湖北影视", "new-hbys" },
    { "437", "湖北教育", "new-hbjy" },
    { "438", "垄上频道", "new-hbls" },
};
#define HBTV_N (sizeof(HBTV_CHANNELS) / sizeof(HBTV_CHANNELS[0]))

typedef struct { char id[8]; char url[1024]; time_t expires; } hbtv_row_t;
static hbtv_row_t g_hbtv_rows[HBTV_N];
static int g_hbtv_rows_n = 0;
static time_t g_hbtv_page_ts = 0;
static pthread_mutex_t g_hbtv_lock = PTHREAD_MUTEX_INITIALIZER;

/* 从频道页 HTML 中解析某频道的 stream URL 并校验（移植 parseChannelPage 的核心） */
static int hbtv_parse_one(const char *html, const hbtv_def_t *def, char *out, size_t outsz, time_t *expires) {
    /* 定位 "rawName" 出现处，向前找最近的 id:<num>，向后找 stream:"..." */
    char namepat[64];
    snprintf(namepat, sizeof namepat, "\"%s\"", def->rawName);
    const char *np = strstr(html, namepat);
    if (!np) return 0;
    /* 校验前面 200 字符内有 id: <def->id> */
    const char *scan = np - 200 > html ? np - 200 : html;
    char idpat[32];
    snprintf(idpat, sizeof idpat, "id:%s", def->id);
    char idpat2[32];
    snprintf(idpat2, sizeof idpat2, "id: %s", def->id);
    const char *q = scan;
    int idok = 0;
    while (q < np) {
        const char *f = strstr(q, "id");
        if (!f || f >= np) break;
        const char *c = f + 2;
        while (c < np && (*c == ' ' || *c == ':')) c++;
        if ((size_t)(np - c) >= strlen(def->id) && strncmp(c, def->id, strlen(def->id)) == 0) { idok = 1; break; }
        q = f + 2;
    }
    if (!idok) return 0;
    /* 向后 500 字符内找 stream */
    const char *sp = strstr(np, "stream");
    if (!sp || sp - np > 500) return 0;
    const char *colon = strchr(sp, ':');
    if (!colon || colon - sp > 20) return 0;
    const char *quote = strchr(colon, '"');
    if (!quote || quote - colon > 20) return 0;
    const char *end = strchr(quote + 1, '"');
    if (!end) return 0;
    /* 取出并去掉 \/ 转义 */
    char url[1024];
    size_t n = 0;
    for (const char *p = quote + 1; p < end && n + 1 < sizeof url; p++) {
        if (*p == '\\' && p + 1 < end && p[1] == '/') { url[n++] = '/'; p++; continue; }
        url[n++] = *p;
    }
    url[n] = 0;
    /* 校验：https + live21-cjy.hbtv.com.cn + /new-hbtv/<streamPath>.m3u8 + auth_key */
    if (strncmp(url, "https://live21-cjy.hbtv.com.cn/new-hbtv/", 40) != 0) return 0;
    char expect[64];
    snprintf(expect, sizeof expect, "/new-hbtv/%s.m3u8", def->streamPath);
    if (strncmp(url + strlen("https://live21-cjy.hbtv.com.cn"), expect, strlen(expect)) != 0) return 0;
    const char *ak = strstr(url, "auth_key=");
    if (!ak) return 0;
    ak += 9;
    long exp = atol(ak);
    if (exp <= time(NULL) + 120) return 0;
    snprintf(out, outsz, "%s", url);
    *expires = exp;
    return 1;
}

static int hbtv_refresh_locked(void) {
    if (g_hbtv_rows_n > 0 && time(NULL) - g_hbtv_page_ts < 600) {
        /* 缓存 10 分钟，且流地址未临期 */
        time_t min_exp = g_hbtv_rows[0].expires;
        for (int i = 0; i < g_hbtv_rows_n; i++)
            if (g_hbtv_rows[i].expires < min_exp) min_exp = g_hbtv_rows[i].expires;
        if (min_exp > time(NULL) + 120) return 0;
    }
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "User-Agent: " HBTV_UA);
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    buf_t body;
    int code = http_fetch("hbtv", HBTV_PAGE_URL, hdrs, 10, &body);
    curl_slist_free_all(hdrs);
    if (code != 200 || !body.data) return -1;
    hbtv_row_t rows[HBTV_N];
    int n = 0;
    for (size_t i = 0; i < HBTV_N; i++) {
        if (hbtv_parse_one(body.data, &HBTV_CHANNELS[i], rows[n].url, sizeof rows[n].url, &rows[n].expires)) {
            snprintf(rows[n].id, sizeof rows[n].id, "%s", HBTV_CHANNELS[i].id);
            n++;
        }
    }
    free(body.data);
    if (n == 0) return -2;
    memcpy(g_hbtv_rows, rows, sizeof rows);
    g_hbtv_rows_n = n;
    g_hbtv_page_ts = time(NULL);
    return 0;
}

static int hbtv_channel_url(const char *id, char *out, size_t outsz) {
    pthread_mutex_lock(&g_hbtv_lock);
    int rc = hbtv_refresh_locked();
    if (rc != 0 && g_hbtv_rows_n == 0) {
        pthread_mutex_unlock(&g_hbtv_lock);
        return -1;
    }
    for (int i = 0; i < g_hbtv_rows_n; i++) {
        if (!strcmp(g_hbtv_rows[i].id, id) && g_hbtv_rows[i].expires > time(NULL) + 60) {
            snprintf(out, outsz, "%s", g_hbtv_rows[i].url);
            pthread_mutex_unlock(&g_hbtv_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_hbtv_lock);
    return -2;
}

/* ---- 分片 key 注册表 ---- */
#define SEG_TABLE_N 8192
typedef struct { char key[17]; char url[2048]; time_t ts; } seg_t;
static seg_t g_seg_table[SEG_TABLE_N];
static pthread_mutex_t g_seg_lock = PTHREAD_MUTEX_INITIALIZER;

static void seg_register(const char *url, char key[17]) {
    char digest[33];
    md5_hex(url, digest);
    memcpy(key, digest, 16);
    key[16] = 0;
    unsigned int hv = 5381;
    for (int i = 0; i < 16; i++) hv = hv * 33 ^ (unsigned char)key[i];
    pthread_mutex_lock(&g_seg_lock);
    unsigned int idx = hv % SEG_TABLE_N;
    for (int probe = 0; probe < 8; probe++) {
        seg_t *s = &g_seg_table[(idx + probe) % SEG_TABLE_N];
        if (!s->key[0] || !strcmp(s->key, key) || time(NULL) - s->ts > 1800) {
            snprintf(s->key, sizeof s->key, "%s", key);
            snprintf(s->url, sizeof s->url, "%s", url);
            s->ts = time(NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_seg_lock);
}

static int seg_lookup(const char *key, char *url, size_t urlsz) {
    unsigned int hv = 5381;
    for (int i = 0; i < 16 && key[i]; i++) hv = hv * 33 ^ (unsigned char)key[i];
    pthread_mutex_lock(&g_seg_lock);
    unsigned int idx = hv % SEG_TABLE_N;
    int found = 0;
    for (int probe = 0; probe < 8; probe++) {
        seg_t *s = &g_seg_table[(idx + probe) % SEG_TABLE_N];
        if (s->key[0] && !strcmp(s->key, key) && time(NULL) - s->ts <= 1800) {
            snprintf(url, urlsz, "%s", s->url);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_seg_lock);
    return found;
}

/* 解析相对 URL（base 为清单地址） */
static void url_resolve(const char *base, const char *rel, char *out, size_t outsz) {
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8)) {
        snprintf(out, outsz, "%s", rel);
        return;
    }
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end) { snprintf(out, outsz, "%s", rel); return; }
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');
    if (rel[0] == '/') {
        if (path_start) snprintf(out, outsz, "%.*s%.*s%s", (int)(scheme_end + 3 - base), base,
                                 (int)(path_start - host_start), host_start, rel);
        else snprintf(out, outsz, "%s%s", base, rel);
        return;
    }
    /* 相对路径：取 base 目录 */
    if (!path_start) { snprintf(out, outsz, "%s/%s", base, rel); return; }
    const char *q = strrchr(path_start, '/');
    const char *qmark = strchr(rel, 0); /* unused */
    (void)qmark;
    /* base 目录截至最后一个 '/'（不含 query 的处理：直接截 path 部分） */
    char dir[1536];
    size_t dlen = (size_t)(q + 1 - base);
    if (dlen >= sizeof dir) dlen = sizeof dir - 1;
    memcpy(dir, base, dlen);
    dir[dlen] = 0;
    /* base 可能带 query，已不影响（q 在 path 内） */
    snprintf(out, outsz, "%s%s", dir, rel);
}

static const char *ext_of(const char *url, char *ext, size_t extsz) {
    const char *p = strrchr(url, '/');
    p = p ? p + 1 : url;
    const char *dot = strrchr(p, '.');
    if (!dot || dot == p) { snprintf(ext, extsz, "ts"); return ext; }
    size_t n = 0;
    for (const char *c = dot + 1; *c && *c != '?' && isalnum((unsigned char)*c) && n + 1 < extsz && n < 8; c++)
        ext[n++] = (char)tolower((unsigned char)*c);
    ext[n] = 0;
    if (!n) snprintf(ext, extsz, "ts");
    return ext;
}

/* 清单获取失败负缓存：同一上游 60 秒内直接失败，防止播放器重试把上游打死、日志刷屏 */
static char g_man_fail_url[1024];
static time_t g_man_fail_ts = 0;
static pthread_mutex_t g_man_lock = PTHREAD_MUTEX_INITIALIZER;

/* 抓取上游清单并改写所有 URI 为 /seg/<key>.<ext>；body 归调用方 free */
static int proxy_manifest(const char *upstream, buf_t *out_body, const char **err) {
    pthread_mutex_lock(&g_man_lock);
    int neg = (g_man_fail_url[0] && !strcmp(g_man_fail_url, upstream) &&
               time(NULL) - g_man_fail_ts < 60);
    pthread_mutex_unlock(&g_man_lock);
    if (neg) { *err = "上游清单获取失败"; return -1; }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Referer: " HBTV_REFERER);
    hdrs = curl_slist_append(hdrs, "User-Agent: " HBTV_UA);
    buf_t raw;
    int code = http_fetch("seg", upstream, hdrs, 10, &raw);
    curl_slist_free_all(hdrs);
    if (code != 200 || !raw.data || !raw.len) {
        free(raw.data);
        pthread_mutex_lock(&g_man_lock);
        snprintf(g_man_fail_url, sizeof g_man_fail_url, "%s", upstream);
        g_man_fail_ts = time(NULL);
        pthread_mutex_unlock(&g_man_lock);
        *err = "上游清单获取失败";
        return -1;
    }
    pthread_mutex_lock(&g_man_lock);
    g_man_fail_url[0] = 0; /* 成功则清除负缓存 */
    pthread_mutex_unlock(&g_man_lock);
    memset(out_body, 0, sizeof *out_body);
    out_body->cap = raw.len * 2 + 4096;
    out_body->data = malloc(out_body->cap);
    if (!out_body->data) { free(raw.data); *err = "内存不足"; return -1; }
    out_body->data[0] = 0;

    char *save = NULL;
    for (char *line = strtok_r(raw.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        size_t ll = strlen(line);
        while (ll && (line[ll-1] == '\r' || line[ll-1] == ' ')) line[--ll] = 0;
        if (!ll) continue;
        if (line[0] == '#') {
            /* 处理 EXT-X-KEY / EXT-X-MEDIA 等标签中的 URI="..." */
            char outl[4096];
            size_t on = 0;
            char *p = line;
            while (*p && on + 1 < sizeof outl) {
                if (strncasecmp(p, "URI=\"", 5) == 0) {
                    char *qe = strchr(p + 5, '"');
                    if (!qe) break;
                    char uri[2048];
                    size_t ul = (size_t)(qe - (p + 5));
                    if (ul >= sizeof uri) ul = sizeof uri - 1;
                    memcpy(uri, p + 5, ul);
                    uri[ul] = 0;
                    char absu[2048], key[17], ext[16];
                    url_resolve(upstream, uri, absu, sizeof absu);
                    seg_register(absu, key);
                    on += snprintf(outl + on, sizeof outl - on, "URI=\"/seg/%s.%s\"", key, ext_of(absu, ext, sizeof ext));
                    p = qe + 1;
                } else {
                    outl[on++] = *p++;
                }
            }
            outl[on] = 0;
            strcat(out_body->data, outl);
            strcat(out_body->data, "\n");
        } else {
            char absu[2048], key[17], ext[16];
            url_resolve(upstream, line, absu, sizeof absu);
            seg_register(absu, key);
            ext_of(absu, ext, sizeof ext);
            size_t need = strlen(out_body->data) + 40;
            if (need < out_body->cap) {
                strcat(out_body->data, "/seg/");
                strcat(out_body->data, key);
                strcat(out_body->data, ".");
                strcat(out_body->data, ext);
                strcat(out_body->data, "\n");
            }
        }
    }
    free(raw.data);
    out_body->len = strlen(out_body->data);
    return 0;
}

/* 分片直流转发：边收边发 */
typedef struct { int fd; int failed; } pipe_ctx_t;
static size_t pipe_write(void *ptr, size_t size, size_t nmemb, void *ud) {
    pipe_ctx_t *pc = ud;
    size_t n = size * nmemb;
    if (pc->failed) return 0;
    if (send_all(pc->fd, ptr, n) < 0) { pc->failed = 1; return 0; }
    return n;
}

static const char *ctype_of_ext(const char *ext) {
    if (!strcmp(ext, "m3u8")) return "application/vnd.apple.mpegurl";
    if (!strcmp(ext, "ts")) return "video/mp2t";
    if (!strcmp(ext, "aac")) return "audio/aac";
    if (!strcmp(ext, "m4s") || !strcmp(ext, "mp4")) return "video/mp4";
    if (!strcmp(ext, "key")) return "application/octet-stream";
    return "application/octet-stream";
}

static void proxy_segment(int fd, const char *url, const char *ext, int head_only) {
    /* 子清单需要二次改写 */
    if (!strcmp(ext, "m3u8")) {
        buf_t body;
        const char *err = NULL;
        if (proxy_manifest(url, &body, &err) != 0) {
            http_respond(fd, 502, "Bad Gateway", "text/plain; charset=utf-8", err ? err : "upstream error");
            return;
        }
        char hdr[512];
        int n = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body.len);
        send_all(fd, hdr, (size_t)n);
        if (!head_only) send_all(fd, body.data, body.len);
        free(body.data);
        return;
    }
    CURL *c = curl_easy_init();
    if (!c) { http_respond(fd, 500, "Internal Server Error", "text/plain", "curl init failed"); return; }
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Referer: " HBTV_REFERER);
    hdrs = curl_slist_append(hdrs, "User-Agent: " HBTV_UA);
    pipe_ctx_t pc = { fd, 0 };
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, pipe_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &pc);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);

    /* 先发响应头（Content-Length 未知，用 chunked 简化：HTTP/1.0 直发 + close） */
    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n", ctype_of_ext(ext));
    send_all(fd, hdr, (size_t)n);
    if (!head_only) curl_easy_perform(c);
    curl_easy_cleanup(c);
    curl_slist_free_all(hdrs);
}

/* ================= 播放列表 ================= */
static void send_playlist(int fd, const char *host) {
    maybe_reload_channels();
    buf_t b;
    memset(&b, 0, sizeof b);
    b.cap = 65536;
    b.data = malloc(b.cap);
    if (!b.data) { http_respond(fd, 500, "Internal Server Error", "text/plain", "oom"); return; }
    b.data[0] = 0;

    char line[8192];
    if (g_epg_url[0])
        snprintf(line, sizeof line,
            "#EXTM3U x-tvg-url=\"%s\" catchup=\"append\" catchup-source=\"?playbackbegin=${(b)yyyyMMddHHmmss}&playbackend=${(e)yyyyMMddHHmmss}\"\n",
            g_epg_url);
    else
        snprintf(line, sizeof line,
            "#EXTM3U catchup=\"append\" catchup-source=\"?playbackbegin=${(b)yyyyMMddHHmmss}&playbackend=${(e)yyyyMMddHHmmss}\"\n");
    strcat(b.data, line);

    pthread_mutex_lock(&g_channels_lock);
    for (int i = 0; i < g_nchannels; i++) {
        channel_t *c = &g_channels[i];
        if (status_get(c->type, c->id) == CH_BAD) continue; /* 不可播的不进列表 */
        snprintf(line, sizeof line,
            "#EXTINF:-1 tvg-id=\"%s\" tvg-name=\"%s\" tvg-logo=\"%s\" group-title=\"%s\",%s\n",
            c->name, c->name, c->logo, c->group, c->name);
        if (strlen(b.data) + strlen(line) + 4096 > b.cap) {
            b.cap *= 2;
            char *nd = realloc(b.data, b.cap);
            if (!nd) break;
            b.data = nd;
        }
        strcat(b.data, line);
        if (!strcmp(c->type, "migu"))
            snprintf(line, sizeof line, "http://%s/migu/%s\n", host, c->id);
        else if (!strcmp(c->type, "hntv"))
            snprintf(line, sizeof line, "http://%s/hntv/%s\n", host, c->id);
        else if (!strcmp(c->type, "hbtv"))
            snprintf(line, sizeof line, "http://%s/hbtv/%s.m3u8\n", host, c->id);
        else
            snprintf(line, sizeof line, "%s\n", c->url);
        strcat(b.data, line);
    }
    pthread_mutex_unlock(&g_channels_lock);
    b.len = strlen(b.data);

    char hdr[512];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: audio/x-mpegurl; charset=utf-8\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", b.len);
    send_all(fd, hdr, (size_t)n);
    send_all(fd, b.data, b.len);
    free(b.data);
}

/* ================= 频道可用性巡检 ================= */
/* 单频道探测：1 可播 0 不可播。direct 是用户自备直连源，不探测默认可播 */
static int check_one(const char *type, const char *id) {
    if (!strcmp(type, "direct")) return 1;
    if (!strcmp(type, "migu")) { char u[4096]; return migu_resolve(id, u, sizeof u) == 0; }
    if (!strcmp(type, "hntv")) { char u[2048]; return hntv_resolve(id, u, sizeof u) == 0; }
    if (!strcmp(type, "hbtv")) {
        char up[1024];
        if (hbtv_channel_url(id, up, sizeof up) != 0) return 0;
        buf_t body; const char *err = NULL;
        if (proxy_manifest(up, &body, &err) == 0) { free(body.data); return 1; }
        return 0;
    }
    return 0;
}

/* 启动后首轮全量检查（不可播的从播放列表剔除）；之后每 10 分钟只复查被剔除的，
   恢复的自动加回。pacing 温柔，避免触发上游风控 */
static void *checker_thread(void *arg) {
    (void)arg;
    sleep(5);
    int round = 0;
    for (;;) {
        typedef struct { char type[8]; char id[64]; } ch_ref_t;
        static ch_ref_t refs[MAX_CHANNELS];
        int n = 0;
        pthread_mutex_lock(&g_channels_lock);
        for (int i = 0; i < g_nchannels && n < MAX_CHANNELS; i++) {
            snprintf(refs[n].type, sizeof refs[n].type, "%s", g_channels[i].type);
            snprintf(refs[n].id, sizeof refs[n].id, "%s", g_channels[i].id);
            n++;
        }
        pthread_mutex_unlock(&g_channels_lock);

        int checked = 0, bad = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(refs[i].type, "direct")) continue;
            if (round > 0 && status_get(refs[i].type, refs[i].id) != CH_BAD) continue;
            int ok = check_one(refs[i].type, refs[i].id);
            status_set(refs[i].type, refs[i].id, ok ? CH_OK : CH_BAD);
            checked++;
            if (!ok) fprintf(stderr, "[iptv] 巡检: %s/%s 不可播放，已从列表剔除\n", refs[i].type, refs[i].id);
            usleep(!strcmp(refs[i].type, "migu") ? 1200 * 1000 : 400 * 1000);
        }
        if (checked) fprintf(stderr, "[iptv] 巡检完成：检查 %d 个，剔除 %d 个\n", checked, bad);
        round++;
        sleep(600);
    }
    return NULL;
}

/* ================= 请求处理 ================= */
static void handle_request(int fd, const char *method, const char *path, const char *host) {
    int head_only = !strcasecmp(method, "HEAD");
    if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {
        http_respond(fd, 405, "Method Not Allowed", "text/plain", "method not allowed");
        return;
    }

    if (!strcmp(path, "/health")) {
        int ok = 0, bad = 0, unk = 0;
        pthread_mutex_lock(&g_channels_lock);
        for (int i = 0; i < g_nchannels; i++) {
            int st = status_get(g_channels[i].type, g_channels[i].id);
            if (st == CH_OK) ok++; else if (st == CH_BAD) bad++; else unk++;
        }
        pthread_mutex_unlock(&g_channels_lock);
        char hb[160];
        snprintf(hb, sizeof hb, "ok | 可播 %d / 剔除 %d / 未检 %d", ok, bad, unk);
        http_respond(fd, 200, "OK", "text/plain; charset=utf-8", hb);
        return;
    }
    if (!strcmp(path, "/") || !strcmp(path, "/interface.m3u") || !strcmp(path, "/iptv.m3u")) {
        send_playlist(fd, host && *host ? host : "127.0.0.1:1905");
        return;
    }
    if (!strncmp(path, "/migu/", 6)) {
        const char *pid = path + 6;
        char pidbuf[32];
        size_t pl = strcspn(pid, "?");
        if (pl >= sizeof pidbuf) pl = sizeof pidbuf - 1;
        memcpy(pidbuf, pid, pl);
        pidbuf[pl] = 0;
        for (const char *c = pidbuf; *c; c++)
            if (!isdigit((unsigned char)*c)) { http_respond(fd, 400, "Bad Request", "text/plain", "bad pid"); return; }
        char final[4096];
        int rc = migu_resolve(pidbuf, final, sizeof final);
        if (rc == 0) {
            status_set("migu", pidbuf, CH_OK);
            http_redirect(fd, final);
        } else {
            if (rc != -4) status_set("migu", pidbuf, CH_BAD); /* -4 是负缓存，前面已记过 */
            http_respond(fd, 502, "Bad Gateway", "text/plain; charset=utf-8", "咪咕取流失败");
        }
        return;
    }
    if (!strncmp(path, "/hntv/", 6)) {
        const char *cid = path + 6;
        char cidbuf[16];
        size_t pl = strcspn(cid, "?");
        if (pl >= sizeof cidbuf) pl = sizeof cidbuf - 1;
        memcpy(cidbuf, cid, pl);
        cidbuf[pl] = 0;
        char final[2048];
        if (hntv_resolve(cidbuf, final, sizeof final) == 0) {
            status_set("hntv", cidbuf, CH_OK);
            http_redirect(fd, final);
        } else {
            status_set("hntv", cidbuf, CH_BAD);
            http_respond(fd, 502, "Bad Gateway", "text/plain; charset=utf-8", "河南台取流失败");
        }
        return;
    }
    if (!strncmp(path, "/hbtv/", 6)) {
        char cidbuf[16];
        const char *cid = path + 6;
        size_t pl = strcspn(cid, ".?");
        if (pl >= sizeof cidbuf) pl = sizeof cidbuf - 1;
        memcpy(cidbuf, cid, pl);
        cidbuf[pl] = 0;
        char upstream[1024];
        if (hbtv_channel_url(cidbuf, upstream, sizeof upstream) != 0) {
            status_set("hbtv", cidbuf, CH_BAD);
            http_respond(fd, 502, "Bad Gateway", "text/plain; charset=utf-8", "湖北台取流失败");
            return;
        }
        buf_t body;
        const char *err = NULL;
        if (proxy_manifest(upstream, &body, &err) != 0) {
            status_set("hbtv", cidbuf, CH_BAD);
            http_respond(fd, 502, "Bad Gateway", "text/plain; charset=utf-8", err ? err : "upstream error");
            return;
        }
        status_set("hbtv", cidbuf, CH_OK);
        char hdr[512];
        int n = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nContent-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", body.len);
        send_all(fd, hdr, (size_t)n);
        if (!head_only) send_all(fd, body.data, body.len);
        free(body.data);
        return;
    }
    if (!strncmp(path, "/seg/", 5)) {
        char key[17], ext[16];
        const char *p = path + 5;
        size_t kl = strcspn(p, ".");
        if (kl != 16) { http_respond(fd, 404, "Not Found", "text/plain", "not found"); return; }
        memcpy(key, p, 16);
        key[16] = 0;
        for (int i = 0; i < 16; i++)
            if (!isxdigit((unsigned char)key[i])) { http_respond(fd, 404, "Not Found", "text/plain", "not found"); return; }
        ext_of(p, ext, sizeof ext);
        char url[2048];
        if (!seg_lookup(key, url, sizeof url)) {
            http_respond(fd, 404, "Not Found", "text/plain; charset=utf-8", "分片不存在或已过期");
            return;
        }
        proxy_segment(fd, url, ext, head_only);
        return;
    }
    http_respond(fd, 404, "Not Found", "text/plain", "not found");
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char req[16384];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof req - 1) {
        ssize_t n = recv(fd, req + total, sizeof req - 1 - (size_t)total, 0);
        if (n <= 0) { close(fd); return NULL; }
        total += n;
        req[total] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char method[8] = "", path[2048] = "", host[256] = "";
    sscanf(req, "%7s %2047s", method, path);
    char *hp = strcasestr(req, "\r\nHost:");
    if (hp) {
        hp += 7;
        while (*hp == ' ' || *hp == '\t') hp++;
        size_t l = strcspn(hp, "\r\n");
        if (l >= sizeof host) l = sizeof host - 1;
        memcpy(host, hp, l);
        host[l] = 0;
    }
    /* 去掉 query 之外的干扰；path 保留 query 供 seg 扩展名解析（其实用不到） */
    char *qm = strchr(path, '?');
    if (qm) *qm = 0;
    if (!path[0]) strcpy(path, "/");

    handle_request(fd, method, path, host);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

/* ================= 自测试 ================= */
static int selftest(void) {
    char m[33], s[65];
    md5_hex("abc", m);
    sha256_hex("abc", s);
    int ok = !strcmp(m, "900150983cd24fb0d6963f7d28e17f72") &&
             !strcmp(s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    printf("md5(abc)=%s\nsha256(abc)=%s\n%s\n", m, s, ok ? "SELFTEST OK" : "SELFTEST FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    srand((unsigned)(time(NULL) ^ (getpid() << 16)));

    /* CA 证书自检：scratch 镜像里这是 HTTPS 出网的命根子，坏了直接喊出来 */
    {
        FILE *ca = fopen(CA_BUNDLE_PATH, "rb");
        if (!ca) {
            fprintf(stderr, "[iptv] 致命：CA 证书 %s 打不开: %s\n", CA_BUNDLE_PATH, strerror(errno));
        } else {
            fseek(ca, 0, SEEK_END);
            long sz = ftell(ca);
            fclose(ca);
            fprintf(stderr, "[iptv] CA 证书 %s (%ld 字节)\n", CA_BUNDLE_PATH, sz);
            if (sz < 10000) fprintf(stderr, "[iptv] 警告：CA 证书文件异常的小，HTTPS 可能失败\n");
        }
    }

    const char *e;
    if ((e = getenv("PORT"))) g_port = atoi(e);
    if ((e = getenv("EPG_URL"))) snprintf(g_epg_url, sizeof g_epg_url, "%s", e);
    if ((e = getenv("MIGU_RATE_TYPE"))) g_rate_type = atoi(e);
    if ((e = getenv("ENABLE_H265"))) g_enable_h265 = !(!strcasecmp(e, "false") || !strcmp(e, "0"));
    if ((e = getenv("ENABLE_HDR"))) g_enable_hdr = !(!strcasecmp(e, "false") || !strcmp(e, "0"));
    if ((e = getenv("MIGU_CACHE_TTL"))) g_migu_cache_ttl = atol(e);
    if ((e = getenv("CHANNELS_FILE"))) snprintf(g_channels_file, sizeof g_channels_file, "%s", e);

    srand((unsigned int)(time(NULL) ^ getpid()));
    signal(SIGPIPE, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    maybe_reload_channels();

    pthread_t th;
    if (pthread_create(&th, NULL, checker_thread, NULL) == 0) pthread_detach(th);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)g_port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 128) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[iptv] 监听端口 %d，频道数 %d\n", g_port, g_nchannels);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); usleep(10000); continue; }
        pthread_t th;
        if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)fd) == 0) {
            pthread_detach(th);
        } else {
            close(fd);
        }
    }
    return 0;
}
