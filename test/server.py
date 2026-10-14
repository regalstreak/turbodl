# Fake file host: /download/<f> -> 302 -> /node/<f>; requires a login cookie; Range; random dropped connections.
import http.server, socketserver, os, random, sys, urllib.parse, time
ROOT=sys.argv[1]; PORT=int(sys.argv[2]); NEED="logged-in-sig=abc"
class H(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def log_message(self,*a): pass
    def do_GET(self):
        if NEED not in (self.headers.get("Cookie") or ""):
            self.send_response(401); self.send_header("Content-Length","0"); self.end_headers(); return
        if self.path.startswith("/download/"):
            self.send_response(302); self.send_header("Location","/node/"+self.path[10:]); self.send_header("Content-Length","0"); self.end_headers(); return
        if not self.path.startswith("/node/"):
            self.send_response(404); self.send_header("Content-Length","0"); self.end_headers(); return
        fn=os.path.join(ROOT,urllib.parse.unquote(self.path[6:])); size=os.path.getsize(fn)
        rg=self.headers.get("Range"); a,b=0,size-1
        if rg:
            a,b=rg.split("=")[1].split("-"); a=int(a); b=int(b) if b else size-1; b=min(b,size-1)
            self.send_response(206); self.send_header("Content-Range",f"bytes {a}-{b}/{size}")
        else: self.send_response(200)
        self.send_header("Content-Length",str(b-a+1)); self.send_header("Connection","close"); self.end_headers()
        with open(fn,"rb") as f:
            f.seek(a); left=b-a+1; drop = random.random()<0.15
            while left>0:
                d=f.read(min(65536,left)); 
                if drop and left < (b-a+1)//2: return   # simulate dropped connection mid-chunk
                self.wfile.write(d); left-=len(d); time.sleep(65536/4e6)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True; allow_reuse_address=True
S(("127.0.0.1",PORT),H).serve_forever()
