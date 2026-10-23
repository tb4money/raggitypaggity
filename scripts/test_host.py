#!/usr/bin/env python3
"""End-to-end test of the CXPS5 API against the host build, using a fake game folder."""
import json, os, shutil, subprocess, sys, tempfile, time, urllib.request, urllib.error, zipfile, io, hashlib

HOST = os.path.abspath(sys.argv[1])
PORT = 18090
tmp = tempfile.mkdtemp(prefix="cxps5test")
game = os.path.join(tmp, "game", "PPSA00000")
data = os.path.join(tmp, "data")
os.makedirs(os.path.join(game, "sce_sys"))
os.makedirs(os.path.join(game, "Media", "StreamingAssets", "aa", "PS5"))
os.makedirs(data)
open(os.path.join(game, "sce_sys", "param.json"), "w").write(
    '{"titleId":"PPSA00000","localizedParameters":{"en-US":{"titleName":"CarX Street"}}}')
open(os.path.join(data, "config.ini"), "w").write(f"port={PORT}\ngame_root={game}\n")

def ufs(engine, payload):
    return b"UnityFS\0" + (6).to_bytes(4, "big") + b"5.x.x\0" + engine.encode() + b"\0" + payload

aa = os.path.join(game, "Media", "StreamingAssets", "aa", "PS5")
orig = {}
for n in ("car_silvia_body.bundle", "car_silvia_audio.bundle", "rim_pack.bundle"):
    orig[n] = ufs("2022.3.10f1", os.urandom(5000))
    open(os.path.join(aa, n), "wb").write(orig[n])
open(os.path.join(aa, "readme.txt"), "w").write("not a bundle")

env = dict(os.environ, CXPS5_DATA=data)
proc = subprocess.Popen([HOST], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
base = f"http://127.0.0.1:{PORT}"

def call(method, path, body=None, raw=False, headers=None):
    data_ = body if isinstance(body, bytes) else (json.dumps(body).encode() if body is not None else (b"" if method in ("POST", "PUT") else None))
    req = urllib.request.Request(base + path, data=data_, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            b = r.read()
            return r.status, (b if raw else json.loads(b))
    except urllib.error.HTTPError as e:
        b = e.read()
        try: return e.code, json.loads(b)
        except Exception: return e.code, b

fails = 0
def check(name, cond, extra=""):
    global fails
    print(("PASS " if cond else "FAIL ") + name + (f"  {extra}" if (extra and not cond) else ""))
    if not cond: fails += 1

try:
    for _ in range(50):
        try: call("GET", "/api/status"); break
        except Exception: time.sleep(0.1)
    st, s = call("GET", "/api/status")
    check("game detected", s["game"]["found"] and s["game"]["titleId"] == "PPSA00000" and s["game"]["name"] == "CarX Street", s)
    check("3 bundles indexed (txt ignored)", s["game"]["bundles"] == 3, s)
    code, ui = call("GET", "/", raw=True)
    check("web UI served", code == 200 and b"CXPS5" in ui)

    # upload: matching bundle with the same engine, a bundle that matches nothing, and junk
    mod1 = ufs("2022.3.10f1", b"MODDED-BODY" * 400)
    code, r = call("PUT", "/api/mods/silvia-ab12/files/car_silvia_body.bundle?modname=Silvia%20S15", mod1)
    check("upload bundle", code == 200, r)
    code, r = call("PUT", "/api/mods/silvia-ab12/files/brand_new_car.bundle", ufs("2021.3.1f1", b"x" * 300))
    check("upload second bundle", code == 200, r)
    code, r = call("PUT", "/api/mods/silvia-ab12/files/junk.bundle", b"this is not unity")
    check("reject non-UnityFS upload", code == 400, r)
    code, r = call("PUT", "/api/mods/silvia-ab12/files/..%2Fevil.bundle", mod1)
    check("reject path traversal name", code == 400, r)
    code, r = call("PUT", "/api/mods/silvia-ab12/files/readme.txt", b"x")
    check("reject non-bundle name", code == 400, r)

    # zip with a nested folder: one deflated, one stored
    zb = io.BytesIO()
    audio = ufs("2022.3.10f1", b"AUDIO" * 3000)
    with zipfile.ZipFile(zb, "w") as z:
        z.writestr("Workshop/1234/car_silvia_audio.bundle", audio, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("Workshop/1234/rim_pack.bundle", ufs("2022.3.10f1", b"R" * 700), compress_type=zipfile.ZIP_STORED)
        z.writestr("Workshop/1234/notes.txt", "ignore me")
    code, r = call("PUT", "/api/mods/audio-pack/zip?modname=Silvia%20audio", zb.getvalue())
    check("zip import extracts 2 bundles", code == 200 and r.get("files") == 2, r)
    check("zip bundle content intact", open(os.path.join(data, "mods", "audio-pack", "car_silvia_audio.bundle"), "rb").read() == audio)

    code, m = call("GET", "/api/mods")
    mods = {x["id"]: x for x in m["mods"]}
    check("two mods listed", set(mods) == {"silvia-ab12", "audio-pack"}, list(mods))
    check("mod name saved", mods["silvia-ab12"]["name"] == "Silvia S15")
    comp = {f["name"]: f["compat"] for f in mods["silvia-ab12"]["files"]}
    check("compat: same engine / no match", comp.get("car_silvia_body.bundle") == "same" and comp.get("brand_new_car.bundle") == "nomatch", comp)
    check("pending after upload", m["pending"] is True)

    # apply
    code, r = call("POST", "/api/apply", {})
    check("apply ok", code == 200 and r["ok"] and r["applied"] == 3 and r["unmatched"] == ["brand_new_car.bundle"], r)
    check("game file replaced", open(os.path.join(aa, "car_silvia_body.bundle"), "rb").read() == mod1)
    check("rim pack replaced", b"R" * 700 in open(os.path.join(aa, "rim_pack.bundle"), "rb").read())
    bk = os.listdir(os.path.join(data, "backup"))
    check("3 backups kept", len(bk) == 3, bk)
    code, m = call("GET", "/api/mods")
    check("not pending after apply", m["pending"] is False, m["pending"])
    code, r = call("POST", "/api/apply", {})
    check("re-apply is a no-op", code == 200 and r["changed"] == [] and r["applied"] == 3, r)

    # priority: a second mod replacing the same file with higher priority wins
    mod2 = ufs("2022.3.10f1", b"HIGHER-PRIORITY" * 100)
    call("PUT", "/api/mods/override-1/files/car_silvia_body.bundle?modname=Override", mod2)
    call("POST", "/api/mods/override-1", {"priority": 5})
    code, r = call("POST", "/api/apply", {})
    check("higher priority wins", open(os.path.join(aa, "car_silvia_body.bundle"), "rb").read() == mod2, r)
    check("backup still the pristine original", any(open(os.path.join(data, "backup", b), "rb").read() == orig["car_silvia_body.bundle"] for b in os.listdir(os.path.join(data, "backup"))))

    # disable -> original returns
    call("POST", "/api/mods/override-1", {"enabled": False})
    code, r = call("POST", "/api/apply", {})
    check("disable falls back to the lower mod", open(os.path.join(aa, "car_silvia_body.bundle"), "rb").read() == mod1, r)
    call("POST", "/api/mods/silvia-ab12", {"enabled": False})
    code, r = call("POST", "/api/apply", {})
    check("disable restores original", open(os.path.join(aa, "car_silvia_body.bundle"), "rb").read() == orig["car_silvia_body.bundle"] and r["restored"] >= 1, r)

    # running-game guard + restore all
    call("POST", "/api/mods/silvia-ab12", {"enabled": True})
    env2 = None
    code, r = call("POST", "/api/apply", {})
    check("re-enable applies", code == 200 and r["ok"], r)
    code, r = call("POST", "/api/restore", {})
    ok = all(open(os.path.join(aa, n), "rb").read() == orig[n] for n in orig)
    check("restore all brings back every original byte-for-byte", code == 200 and ok, r)
    check("backups cleaned up", os.listdir(os.path.join(data, "backup")) == [])

    # diagnostics: a mod built with another Unity version is flagged, and the report downloads
    call("PUT", "/api/mods/odd-1/files/rim_pack.bundle?modname=Odd%20rims", ufs("2019.4.1f1", b"o" * 500))
    code, r = call("GET", "/api/diag")
    check("diag flags Unity mismatch", code == 200 and any(l.startswith("WARN") and "2019.4.1f1" in l for l in r["findings"]), r)
    call("POST", "/api/apply", {})
    code, rep = call("GET", "/api/report", raw=True)
    rep = rep.decode()
    check("report has sections", code == 200 and "== Game ==" in rep and "== Findings ==" in rep and "== Log" in rep and "Version:" in rep, rep[:200])
    check("report lists swapped files and applied log", "rim_pack.bundle  <-  mod" in rep and "applied rim_pack.bundle" in rep)
    call("POST", "/api/restore", {})
    call("DELETE", "/api/mods/odd-1")

    # delete
    code, r = call("DELETE", "/api/mods/override-1")
    code, m = call("GET", "/api/mods")
    check("delete removes mod", "override-1" not in [x["id"] for x in m["mods"]])
    code, r = call("GET", "/api/nope")
    check("unknown endpoint 404", code == 404)
finally:
    proc.terminate()
    proc.wait()

# running-game guard (separate process so the env flag applies)
env = dict(os.environ, CXPS5_DATA=data, CXPS5_FAKE_RUNNING="1")
proc = subprocess.Popen([HOST], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try: call("GET", "/api/status"); break
        except Exception: time.sleep(0.1)
    code, r = call("POST", "/api/apply", {})
    check("apply refused while the game runs", code == 409, r)
    code, r = call("POST", "/api/apply", {"force": True})
    check("force overrides the guard", code == 200, r)
finally:
    proc.terminate(); proc.wait()

# PIN protection
open(os.path.join(data, "config.ini"), "a").write("pin=1234\n")
env = dict(os.environ, CXPS5_DATA=data)
proc = subprocess.Popen([HOST], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try: call("GET", "/"); break
        except Exception: time.sleep(0.1)
    code, r = call("GET", "/api/status")
    check("PIN required without header", code == 401, r)
    code, r = call("GET", "/api/status", headers={"X-CXPS5-PIN": "1234"})
    check("PIN accepted", code == 200)
finally:
    proc.terminate(); proc.wait()

shutil.rmtree(tmp, ignore_errors=True)
print("\n%s" % ("ALL PASSED" if not fails else f"{fails} FAILED"))
sys.exit(1 if fails else 0)
