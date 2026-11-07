#!/usr/bin/env python3
"""Autoriza o Robot AI a acessar a Google Agenda e grava GOOGLE_REFRESH_TOKEN no .env.

Uso (no PC, uma unica vez):
    python3 scripts/google_auth.py

Requer GOOGLE_CLIENT_ID e GOOGLE_CLIENT_SECRET no .env, de uma credencial OAuth do tipo
"App para computador" (Desktop app). O fluxo de dispositivo (TV) do Google nao permite o
escopo da Agenda, por isso a autorizacao e feita aqui e o token e usado pelo ESP32.
"""
import base64, hashlib, http.server, json, os, re, secrets, threading, urllib.parse, urllib.request, webbrowser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(ROOT, ".env")
SCOPE = "https://www.googleapis.com/auth/calendar.events"


def read_env():
    d = {}
    with open(ENV, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                d[k.strip()] = v.strip().strip("'\"")
    return d


def write_env(key, value):
    with open(ENV, encoding="utf-8") as f:
        text = f.read()
    line = f'{key}="{value}"'
    if re.search(rf"^{key}=.*$", text, re.M):
        text = re.sub(rf"^{key}=.*$", lambda m: line, text, flags=re.M)
    else:
        text = text.rstrip("\n") + "\n" + line + "\n"
    with open(ENV, "w", encoding="utf-8") as f:
        f.write(text)
    os.chmod(ENV, 0o600)


def main():
    env = read_env()
    cid, secret = env.get("GOOGLE_CLIENT_ID"), env.get("GOOGLE_CLIENT_SECRET")
    if not cid or not secret:
        raise SystemExit("Defina GOOGLE_CLIENT_ID e GOOGLE_CLIENT_SECRET no .env")

    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    result = {}

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if q.get("state", [""])[0] == state and "code" in q:
                result["code"] = q["code"][0]
                msg = "Autorizado! Pode fechar esta aba e voltar ao terminal."
            else:
                result["error"] = q.get("error", ["desconhecido"])[0]
                msg = "Falha na autorizacao: " + result["error"]
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(msg.encode())
            threading.Thread(target=self.server.shutdown, daemon=True).start()

        def log_message(self, *a):
            pass

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    redirect = f"http://127.0.0.1:{srv.server_port}"
    url = "https://accounts.google.com/o/oauth2/v2/auth?" + urllib.parse.urlencode({
        "client_id": cid, "redirect_uri": redirect, "response_type": "code", "scope": SCOPE,
        "access_type": "offline", "prompt": "consent", "state": state,
        "code_challenge": challenge, "code_challenge_method": "S256"})
    print("Abra este link e autorize (tentando abrir o navegador):\n" + url + "\n")
    webbrowser.open(url)
    srv.serve_forever()

    if "code" not in result:
        raise SystemExit("Autorizacao nao concluida: " + result.get("error", "?"))
    data = urllib.parse.urlencode({
        "client_id": cid, "client_secret": secret, "code": result["code"], "redirect_uri": redirect,
        "grant_type": "authorization_code", "code_verifier": verifier}).encode()
    try:
        tok = json.loads(urllib.request.urlopen("https://oauth2.googleapis.com/token", data=data, timeout=30).read())
    except urllib.error.HTTPError as e:
        raise SystemExit("Erro ao trocar o codigo: " + e.read().decode())
    if "refresh_token" not in tok:
        raise SystemExit("O Google nao devolveu refresh_token. Remova o acesso do app em myaccount.google.com/permissions e rode de novo.")
    write_env("GOOGLE_REFRESH_TOKEN", tok["refresh_token"])
    print("Pronto! GOOGLE_REFRESH_TOKEN gravado no .env. Agora rode: pio run -t upload")


if __name__ == "__main__":
    main()
