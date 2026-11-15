#!/usr/bin/env python3
# Skyline - Copyright (C) 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
# Builds, signs and publishes a Skyline release from the server, in one go:
#   skyline-release                          (notes from release-notes.txt, or "Skyline x.y")
#   skyline-release "What's new" "Hva er nytt"
#   skyline-release --dry-run                (build + sign into a temp folder, publish nothing)
#
# The version comes from src/version.h; bump it there first. Steps: build (no warnings
# allowed) -> check the exe's version and built-in key -> sign latest.txt with the
# private key -> update folder + download page (latest.txt last) -> download everything
# back over HTTPS and compare -> copy the exe to ferdig/. Same manifest format and signature
# (ECDSA P-256, SHA-256, r||s) as tools/sign-release.ps1.
#
# release-notes.txt (optional, repo root): first line English, second line Norwegian.
import argparse, fcntl, hashlib, os, re, shutil, subprocess, sys, tempfile, time, urllib.request, zipfile
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature
from cryptography.exceptions import InvalidSignature

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = '/home/web/web/skybyn.com/public_html/skyline'
UPDATE_DIR = SITE + '/update'
BASE_URL = 'https://skyline.skybyn.com'
KEY_FILE = '/home/cyberbyte/skyline-signing.key'
TOOLCHAIN = '/opt/llvm-mingw/bin'
WEB_USER = 'web'


def die(msg):
    sys.exit('skyline-release: ' + msg)


def step(msg):
    print('==> ' + msg, flush=True)


def read_version():
    t = open(os.path.join(REPO, 'src/version.h'), encoding='utf-8').read()
    parts = [re.search(r'#define SKYLINE_VERSION_%s (\d+)' % n, t) for n in ('MAJOR', 'MINOR', 'PATCH')]
    if not all(parts):
        die("couldn't read the version from src/version.h")
    v = tuple(int(p.group(1)) for p in parts)
    s = re.search(r'#define SKYLINE_VERSION_STR "([^"]+)"', t)
    if not s or s.group(1) != '%d.%d.%d' % v:
        die('src/version.h: SKYLINE_VERSION_STR does not match MAJOR/MINOR/PATCH')
    return v


def built_in_key():
    t = open(os.path.join(REPO, 'src/update_key.h'), encoding='utf-8').read()
    b = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]{2})', t))
    if len(b) != 72 or not any(b[8:]):
        die('src/update_key.h has no key')
    return b


def load_private_key(path):
    try:
        blob = open(path, 'rb').read()
    except OSError as e:
        die('no signing key at %s (%s)' % (path, e.strerror))
    # BCRYPT_ECCPRIVATE_BLOB: "ECS2", cbKey=32, X, Y, d
    if len(blob) != 104 or blob[:4] != b'ECS2' or int.from_bytes(blob[4:8], 'little') != 32:
        die('%s is not a Skyline signing key' % path)
    x, y, d = (int.from_bytes(blob[o:o + 32], 'big') for o in (8, 40, 72))
    key = ec.derive_private_key(d, ec.SECP256R1())
    pub = key.public_key().public_numbers()
    if (pub.x, pub.y) != (x, y):
        die('%s is damaged (public half does not match)' % path)
    public_blob = b'ECS1' + (32).to_bytes(4, 'little') + blob[8:72]
    return key, public_blob


def sign(key, data):
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, 'big') + s.to_bytes(32, 'big')


def verify(public_blob, data, sig):
    x, y = (int.from_bytes(public_blob[o:o + 32], 'big') for o in (8, 40))
    pub = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
    try:
        pub.verify(encode_dss_signature(int.from_bytes(sig[:32], 'big'), int.from_bytes(sig[32:], 'big')),
                   data, ec.ECDSA(hashes.SHA256()))
        return True
    except InvalidSignature:
        return False


def exe_version(data):
    i = data.find('ProductVersion'.encode('utf-16le'))
    if i < 0:
        return None
    m = re.search(r'(\d+\.\d+\.\d+)', data[i + 28:i + 120].decode('utf-16le', 'ignore'))
    return m.group(1) if m else None


def published_version():
    try:
        t = open(os.path.join(UPDATE_DIR, 'latest.txt'), encoding='utf-8').read()
    except FileNotFoundError:
        return None
    m = re.search(r'^version=(\d+)\.(\d+)\.(\d+)$', t, re.M)
    return tuple(int(g) for g in m.groups()) if m else None


def first_line(path):
    with open(path, encoding='utf-8-sig') as f:
        return f.readline().strip()


def fetch(path):
    url = '%s/%s?nocache=%d' % (BASE_URL, path, time.time_ns())
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'skyline-release'}), timeout=60) as r:
        return r.read()


def put(src, dst, data=None):
    """Write a site file as the web user, atomically (temp file + rename)."""
    tmp = dst + '.tmp-release'
    if data is None:
        shutil.copyfile(src, tmp)
    else:
        with open(tmp, 'wb') as f:
            f.write(data)
    shutil.chown(tmp, WEB_USER, WEB_USER)
    os.chmod(tmp, 0o644)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser(description='Build, sign and publish a Skyline release.')
    ap.add_argument('notes', nargs='?', help="what's new, English (one line)")
    ap.add_argument('notes_no', nargs='?', help='hva er nytt, norsk (en linje)')
    ap.add_argument('--dry-run', action='store_true', help='build and sign into a temp folder, publish nothing')
    ap.add_argument('--key', default=KEY_FILE, help='private signing key (default %(default)s)')
    a = ap.parse_args()
    if os.geteuid() != 0:
        die('run as root (sudo)')

    lock = open('/tmp/skyline-release.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        die('another release is already running')

    v = read_version()
    ver = '%d.%d.%d' % v
    disp = '%d.%d' % v[:2] if v[2] == 0 else ver   # "2.4" / "2.4.1", as in the docs and zip name
    step('Skyline %s' % ver)

    pub_v = published_version()
    if pub_v and v <= pub_v and not a.dry_run:
        die('%s is already published (latest is %s). Bump the version in src/version.h first.' % (ver, '.'.join(map(str, pub_v))))

    notes, notes_no = a.notes, a.notes_no
    rn = os.path.join(REPO, 'release-notes.txt')
    if notes is None and os.path.exists(rn):
        lines = [l.strip() for l in open(rn, encoding='utf-8-sig') if l.strip()]
        notes = lines[0] if lines else None
        notes_no = notes_no or (lines[1] if len(lines) > 1 else None)
    notes = ' '.join((notes or 'Skyline ' + disp).split())
    notes_no = ' '.join((notes_no or notes).split())

    ferdig = os.path.join(REPO, 'ferdig')
    docs = {'README.txt': 'README.txt', 'LES MEG.txt': 'LES MEG.txt', 'LICENSE.txt': 'LICENSE.txt'}
    for name in ('README.txt', 'LES MEG.txt', 'LES-MEG.md'):
        p = os.path.join(ferdig, name)
        if not os.path.exists(p):
            die('ferdig/%s is missing' % name)
        if disp not in first_line(p):
            die('ferdig/%s still says "%s"; update the docs to %s first' % (name, first_line(p), disp))

    key, public_blob = load_private_key(a.key)
    if public_blob != built_in_key():
        die("the signing key doesn't match src/update_key.h (wrong key file?)")

    step('Building')
    env = dict(os.environ, PATH=TOOLCHAIN + ':' + os.environ.get('PATH', ''))
    b = subprocess.run(['sh', './build.sh'], cwd=REPO, env=env, capture_output=True, text=True)
    if b.returncode != 0 or re.search(r'\b(warning|error):', b.stdout + b.stderr):
        print(b.stdout + b.stderr)
        die('build failed or has warnings')
    exe = open(os.path.join(REPO, 'out/Skyline.exe'), 'rb').read()
    if exe_version(exe) != ver:
        die('the exe says version %s, expected %s' % (exe_version(exe), ver))
    if public_blob not in exe:
        die("the exe doesn't contain the signing key")
    sha = hashlib.sha256(exe).hexdigest()

    step('Signing')
    name = 'Skyline-%s.exe' % ver
    body = ('skyline-update 1\nversion=%s\nurl=%s/update/%s\nsize=%d\nsha256=%s\nnotes=%s\nnotes_no=%s\n'
            % (ver, BASE_URL, name, len(exe), sha, notes, notes_no)).encode('utf-8')
    sig = sign(key, body)
    if not verify(public_blob, body, sig):
        die('signing failed')
    manifest = body + b'sig=' + sig.hex().encode() + b'\n'

    out = tempfile.mkdtemp(prefix='skyline-release-') if a.dry_run else os.path.join(REPO, 'release')
    os.makedirs(out, exist_ok=True)
    shutil.copyfile(os.path.join(REPO, 'out/Skyline.exe'), os.path.join(out, name))
    open(os.path.join(out, 'latest.txt'), 'wb').write(manifest)

    zname = 'Skyline-%s.zip' % disp
    zpath = os.path.join(out, zname)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr('Skyline/', b'')
        z.write(os.path.join(REPO, 'out/Skyline.exe'), 'Skyline/Skyline.exe')
        for src, dst in docs.items():
            z.write(os.path.join(ferdig, src), 'Skyline/' + dst)

    page = open(os.path.join(SITE, 'index.html'), encoding='utf-8').read()
    subs = [(r'href="Skyline-[0-9.]+\.zip" download>Download Skyline [0-9.]+<',
             'href="%s" download>Download Skyline %s<' % (zname, disp)),
            (r'· \d+ KB zip', '· %d KB zip' % round(os.path.getsize(zpath) / 1024)),
            (r'>exe</a> \(\d+ KB\)', '>exe</a> (%d KB)' % round(len(exe) / 1024)),
            (r'SHA-256 of Skyline\.exe [0-9.]+:', 'SHA-256 of Skyline.exe %s:' % disp),
            (r'<span class="hash">[0-9a-f]{64}</span>', '<span class="hash">%s</span>' % sha)]
    for pat, rep in subs:
        page, n = re.subn(pat, lambda m: rep, page)
        if n != 1:
            die('index.html: expected one match for %s, found %d' % (pat, n))

    if a.dry_run:
        open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(page)
        step('Dry run OK, nothing published. Files in %s' % out)
        print('    sha256 %s\n    notes  %s / %s' % (sha, notes, notes_no))
        return

    step('Publishing')
    shutil.copyfile(os.path.join(SITE, 'index.html'), os.path.join(out, 'index-before-%s.html' % disp))
    put(os.path.join(out, name), os.path.join(UPDATE_DIR, name))
    put(zpath, os.path.join(SITE, zname))
    put(os.path.join(REPO, 'out/Skyline.exe'), os.path.join(SITE, 'Skyline.exe'))
    for doc in ('README.txt', 'LES-MEG.md', 'LICENSE.txt'):
        put(os.path.join(ferdig, doc), os.path.join(SITE, doc))
    put(None, os.path.join(SITE, 'index.html'), page.encode('utf-8'))
    put(None, os.path.join(UPDATE_DIR, 'latest.txt'), manifest)   # last: this is what installed copies see

    step('Checking the live site')
    problems = []
    if fetch('update/latest.txt') != manifest: problems.append('update/latest.txt')
    if hashlib.sha256(fetch('update/' + name)).hexdigest() != sha: problems.append('update/' + name)
    if hashlib.sha256(fetch('Skyline.exe')).hexdigest() != sha: problems.append('Skyline.exe')
    zf = os.path.join(out, 'check.zip')
    open(zf, 'wb').write(fetch(zname))
    with zipfile.ZipFile(zf) as z:
        if hashlib.sha256(z.read('Skyline/Skyline.exe')).hexdigest() != sha: problems.append(zname)
    os.remove(zf)
    if sha not in fetch('').decode('utf-8', 'ignore'): problems.append('index.html')
    if problems:
        die('published, but these do not match on the live site: ' + ', '.join(problems))

    shutil.copyfile(os.path.join(REPO, 'out/Skyline.exe'), os.path.join(ferdig, 'Skyline.exe'))

    step('Done: Skyline %s is live' % disp)
    print('    download  %s/\n    update    %s/update/latest.txt\n    sha256    %s\n    notes     %s / %s'
          % (BASE_URL, BASE_URL, sha, notes, notes_no))


if __name__ == '__main__':
    main()
