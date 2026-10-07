# iptv-c

[akiralereal/iptv](https://github.com/akiralereal/iptv) 的极简 C 语言重实现。
原版是 Node.js + 无头 Chromium 的全功能管理系统（常驻内存数百 MB）；
本项目只保留「**取流分发**」这一核心功能，单文件 C + libcurl，**常驻内存约 5MB**。

## 功能

| 端点 | 说明 |
| --- | --- |
| `GET /interface.m3u`（或 `/`） | 输出播放列表（频道来自 `channels.conf`，改文件即热加载） |
| `GET /migu/<pID>` | 咪咕取流（720p 签名算法）→ 302 到 CDN m3u8，缓存 3 小时 |
| `GET /hntv/<cid>` | 河南台（大象新闻，SHA-256 请求签名）→ 302 |
| `GET /hbtv/<id>.m3u8` | 湖北台（长江云）HLS 全代理：清单改写 + 分片转发（带官网 Referer） |
| `GET /seg/<key>.<ext>` | 湖北台分片/子清单代理（播放器自动访问，无需手动调） |
| `GET /health` | 健康检查 |

频道类型（`channels.conf` 每行 `type|id|name|group|logo|url`）：

- `migu` — 咪咕源，id 为 pID（央视、卫视等绝大多数频道）
- `hntv` — 河南台，id 为官网 cid
- `hbtv` — 湖北台，id 为 431/432/433/435/437/438
- `direct` — 直连地址，url 填完整 m3u8

## 部署

```bash
docker compose up -d --build
```

播放列表地址：`http://<主机IP>:1905/interface.m3u`

环境变量见 `docker-compose.yml` 注释（端口、画质、H265/HDR 开关、EPG 地址、缓存时长）。

镜像说明：静态编译（libcurl 全部静态链接）+ `scratch` 运行镜像，约 4MB、无任何动态库依赖；
构建期每个架构自动跑 `--selftest` 校验 MD5/SHA-256 签名算法。注意 scratch 镜像没有 shell，
`docker exec` 进不去，日志用 `docker logs iptv-c` 看。

## GitHub Actions 编译多架构镜像

1. 把本目录推到 GitHub 仓库，push 到 main 分支即自动编译
   `linux/amd64, linux/arm64, linux/arm/v7` 并推送到 GitHub Container Registry：
   `ghcr.io/<你的github用户名>/iptv-c:latest`（用仓库自动的 `GITHUB_TOKEN`，无需配任何 secret）
2. 首次推送后包默认是私有的，需改公开一次：
   GitHub 个人主页 → Packages → `iptv-c` → Package settings → Change visibility → Public
   （也可以在仓库首页右侧 Packages 里点进去设置）

NAS / 玩客云使用编译好的镜像时，把 compose 里的 `build: .` 换成
`image: ghcr.io/<你的github用户名>/iptv-c:latest`；国内拉不动可换镜像前缀
`ghcr.nju.edu.cn/<你的github用户名>/iptv-c:latest`。

## 与原版相比砍掉的东西

管理后台、用户系统、访问密码、EPG 聚合生成（可用 `EPG_URL` 指到公共节目单）、
央视频/四川广电等需要无头浏览器的抓取源、台标缓存服务、回放数据生成。
取流签名逻辑（咪咕 ddCalcu / 大象新闻 sign / 长江云页面解析）与原版一致。

## 声明

本项目仅为自托管工具，不托管任何音视频内容；请仅在获得合法授权的范围内使用。
