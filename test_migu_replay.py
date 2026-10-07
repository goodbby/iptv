# 复刻 C 版 migu playurl 请求，对比「固定 salt」（C 未播种 rand 的缺陷）与「随机 salt」
import hashlib, time, random, subprocess, sys, json

IP = "218.98.16.22"
PID = "608807420"

def build(salt):
    ts = str(int(time.time() * 1000))
    appver = "2600034600"
    str1 = ts + PID + appver[:8]
    md5_1 = hashlib.md5(str1.encode()).hexdigest()
    suffix = "2cac4f2c6c3346a5b34e085725ef7e33migu" + salt[:4]
    sign = hashlib.md5((md5_1 + suffix).encode()).hexdigest()
    url = (f"https://play.miguvideo.com/playurl/v1/play/playurl?sign={sign}"
           f"&rateType=3&contId={PID}&timestamp={ts}&salt={salt}"
           f"&flvEnable=true&super4k=true")
    return url

def fetch(url):
    r = subprocess.run([
        "curl", "-s", "--max-time", "12",
        "--resolve", f"play.miguvideo.com:443:{IP}",
        "-H", "AppVersion: 2600034600",
        "-H", "TerminalId: android",
        "-H", "X-UP-CLIENT-CHANNEL-ID: 2600034600-99000-201600010010028",
        "-H", "appCode: miguvideo_default_android",
        "-w", "\n__HTTP__%{http_code}",
        url], capture_output=True, text=True, timeout=20)
    return r.stdout

# C 程序未播种时 glibc rand() 第一次返回值固定为 1804289383 → salt 恒为 "38938325"
fixed_salt = "38938325"
rand_salt = f"{random.randint(0, 999999):06d}25"

for label, salt in [("固定salt(模拟C程序)", fixed_salt), ("随机salt(对照组)", rand_salt)]:
    out = fetch(build(salt))
    body, _, tail = out.rpartition("__HTTP__")
    print(f"===== {label} salt={salt} HTTP={tail.strip()} =====")
    print(body[:600].replace("\n", " "))
    print()
