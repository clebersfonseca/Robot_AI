import os
import re

Import("env")

# Lista estrita (whitelist) de variáveis permitidas do .env para evitar injeção de flags no compilador
ALLOWED_KEYS = {
    "WIFI_SSID",
    "WIFI_PASS",
    "WIFI_PASSWORD",
    "GEMINI_API_KEY",
    "GEMINI_MODEL",
    "GEMINI_TTS_MODEL",
    "GOOGLE_CLIENT_ID",
    "GOOGLE_CLIENT_SECRET",
    "GOOGLE_REFRESH_TOKEN",
}

env_file = os.path.join(env.get("PROJECT_DIR"), ".env")

if os.path.exists(env_file):
    # Garante permissão restrita (0600) no arquivo .env em sistemas POSIX
    try:
        os.chmod(env_file, 0o600)
    except OSError:
        pass

    with open(env_file, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            # Ignora linhas vazias ou comentários
            if not line or line.startswith("#") or "=" not in line:
                continue

            key, val = line.split("=", 1)
            key = key.strip()
            if key not in ALLOWED_KEYS or not re.match(r"^[A-Z0-9_]+$", key):
                continue

            val = val.strip().strip("'\"")
            # Escapa barras e aspas para evitar quebra ou injeção de macro C++
            val = val.replace("\\", "\\\\").replace('"', '\\"')

            # Injeta a macro como string C++: -DKEY=\"VAL\"
            env.Append(CPPDEFINES=[(key, f'\\"{val}\\"')])
else:
    print("\n[AVISO] Arquivo .env não encontrado! Usando valores padrão ou falhará na compilação.\n")