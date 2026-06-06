import serial, time, base64, hashlib, sys

PORT, BAUD = "/dev/cu.usbserial-210", 115200
BIN = "/tmp/hello"; REMOTE = "/tmp/hello"

data = open(BIN, "rb").read()
b64 = base64.b64encode(data).decode()
md5 = hashlib.md5(data).hexdigest()
print(f"local: {len(data)} bytes md5={md5}")

ser = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.3)

def _read_until(tok, t):
    end=time.time()+t; buf=b""; tb=tok.encode()
    while time.time()<end:
        n=ser.in_waiting
        if n:
            buf+=ser.read(n)
            if tb in buf: return buf
            end=time.time()+0.4
        else: time.sleep(0.02)
    return buf

# Sentinels are quote-split in the command so the ECHOED command never contains
# the literal token -- only the real printed output does.
def run(c, t=6.0, show=True):
    ser.reset_input_buffer()
    line = f"echo 'B''EGIN'; {c}; echo 'E''ND_'$?\r\n"
    ser.write(line.encode())
    buf=_read_until("END_", t).decode("utf-8","replace")
    out=""
    if "BEGIN" in buf:
        after=buf.split("BEGIN",1)[1].lstrip("\r\n")
        out=after.split("END_",1)[0].rstrip()
    rc=None
    if "END_" in buf:
        tail=buf.split("END_",1)[1].strip()
        rc=tail.split()[0] if tail.split() else "?"
    if show: print(f"$ {c}\n{out}    [rc={rc}]")
    return out, rc

ser.write(b"\r\n"); time.sleep(0.4); ser.reset_input_buffer()

# --- pick a decode method ---
print("\n--- capability probe (fixed protocol) ---")
b64ok,_ = run("printf aGVsbG8= | busybox base64 -d 2>&1")
octok,_ = run(r"printf '\101\102\103'")
method = "base64" if b64ok.strip()=="hello" else ("octal" if octok.strip()=="ABC" else None)
print(f"\n==> chosen transfer method: {method}")
if method is None:
    print("Neither base64 nor octal-printf works; aborting."); ser.close(); sys.exit(1)

# --- build payload + chunks ---
if method=="base64":
    payload=b64; decode_cmd=f"cat {REMOTE}.enc | busybox base64 -d > {REMOTE}"
else:
    payload="".join("\\%03o"%x for x in data); decode_cmd=None  # octal writes raw directly

run(f"rm -f {REMOTE} {REMOTE}.enc", show=False)

print("\n--- transferring ---")
CHUNK=180; n=(len(payload)+CHUNK-1)//CHUNK
target = f"{REMOTE}.enc" if method=="base64" else REMOTE
for i in range(0,len(payload),CHUNK):
    part=payload[i:i+CHUNK]
    run(f"printf %s '{part}' >> {target}" if method=="base64"
        else f"printf '{part}' >> {target}", t=4.0, show=False)
    if (i//CHUNK)%10==0:
        sys.stdout.write(f"\r  {i//CHUNK+1}/{n} chunks"); sys.stdout.flush()
print(f"\r  {n}/{n} chunks sent")

if decode_cmd: run(decode_cmd, show=False)
run(f"chmod +x {REMOTE}", show=False)

print("\n--- verify on board ---")
run(f"wc -c {REMOTE}")
run(f"busybox md5sum {REMOTE}")
print(f"(expect {len(data)} bytes, md5={md5})")

print("\n--- EXECUTE on V831 ---")
out,rc = run(REMOTE, t=6.0)
print(f"\n>>> RESULT: {out!r}  (exit {rc})")
ser.close()
