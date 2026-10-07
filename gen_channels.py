# -*- coding: utf-8 -*-
# 解析 interface.orig.m3u，生成 channels.conf（格式: type|id|name|group|logo|url）
# 台标来源：咪咕接口（pics.highResolutionH）+ 原服务 /api/channels 备份（channels.api.json）
import json
import re
import urllib.request

UA = {"User-Agent": "Mozilla/5.0"}


def fetch_json(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode("utf-8"))


# 1. 咪咕全量频道: pid -> logo
migu_logo = {}
cates = fetch_json(
    "https://program-sc.miguvideo.com/live/v2/tv-data/1ff892f2b5ab4a79be6e25b69d2f5d05"
)["body"]["liveList"]
for cate in cates:
    if cate.get("name") == "热门":
        continue
    try:
        data = fetch_json(
            "https://program-sc.miguvideo.com/live/v2/tv-data/" + cate["vomsID"]
        )
    except Exception as e:
        print("分类抓取失败:", cate.get("name"), e)
        continue
    for p in data.get("body", {}).get("dataList", []):
        pid = str(p.get("pID", ""))
        logo = (p.get("pics") or {}).get("highResolutionH", "")
        if pid and pid not in migu_logo:
            migu_logo[pid] = logo

# 2. 原服务频道数据（可选）：name -> logo，给 hntv/hbtv/direct 补台标
name_logo = {}
try:
    api = json.load(open("channels.api.json", encoding="utf-8"))
    for cate in api:
        for ch in cate.get("dataList", []):
            logo = ch.get("logo") or (ch.get("pics") or {}).get("highResolutionH", "")
            if ch.get("name") and logo:
                name_logo.setdefault(ch["name"], logo)
except FileNotFoundError:
    print("提示: 无 channels.api.json，hntv/hbtv 台标将为空")

# 3. 解析用户 playlist
entries = []
with open("interface.orig.m3u", encoding="utf-8") as f:
    lines = [l.strip() for l in f if l.strip()]
for i, line in enumerate(lines):
    if line.startswith("#EXTINF"):
        name = line.rsplit(",", 1)[-1]
        m = re.search(r'group-title="([^"]*)"', line)
        group = m.group(1) if m else ""
        entries.append((name, group, lines[i + 1]))

# 4. 生成 channels.conf
n = 0
with open("channels.conf", "w", encoding="utf-8") as f:
    f.write("# 频道配置: type|id|name|group|logo|url\n")
    f.write("# type: migu(咪咕, id=pID) / hntv(河南, id=cid) / hbtv(湖北, id=频道号) / direct(直连, url=完整地址)\n")
    for name, group, url in entries:
        tail = url.rstrip("/").rsplit("/", 1)[-1]
        if re.fullmatch(r"\d{6,}", tail):
            f.write(f"migu|{tail}|{name}|{group}|{migu_logo.get(tail, name_logo.get(name, ''))}|\n")
        elif tail.startswith("hntv-"):
            cid = tail[5:]
            f.write(f"hntv|{cid}|{name}|{group}|{name_logo.get(name, '')}|\n")
        elif re.fullmatch(r"hbtv-\d{3}(?:\.m3u8)?", tail):
            cid = tail.replace("hbtv-", "").replace(".m3u8", "")
            f.write(f"hbtv|{cid}|{name}|{group}|{name_logo.get(name, '')}|\n")
        else:
            f.write(f"direct|-|{name}|{group}|{name_logo.get(name, '')}|{url}\n")
        n += 1

print("写入频道数:", n)
