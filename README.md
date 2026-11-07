# Robot AI — Waveshare ESP32-S3-Touch-AMOLED-2.16 + Google Gemini

Assistente de IA por voz standalone com avatar robô animado em pixel art, desenvolvido sob medida para a placa de desenvolvimento **Waveshare ESP32-S3 2.16 inch AMOLED Touch Display Board (N16R8)**.

O sistema comunica-se diretamente com as APIs do **Google Gemini** (`gemini-3.5-flash-lite` para raciocínio/compreensão e `gemini-3.1-flash-tts-preview` com voz *"Kore"* para síntese de áudio), realizando toda a captura de voz, processamento e reprodução de áudio diretamente no hardware do dispositivo, sem depender de servidor intermediário no PC.

---

## 🌟 Recursos Principais

- **100% Standalone no ESP32-S3**:
  - Comunica-se diretamente com `https://generativelanguage.googleapis.com` via **HTTPS/TLS seguro**, validando os certificados raiz oficiais da Google (`GTS Root R1` e `GTS Root R4`).
  - Sem necessidade de servidores locais ou pontes adicionais.
- **Áudio Bidirecional Integrado (`ES8311` + `ES7210`)**:
  - **Entrada (Microfone)**: Captura voz pelo codec ADC **ES7210** (`0x40`) a 16 kHz 16-bit mono com ganho ajustado e cancelamento de ruído.
  - **Saída (Alto-Falante)**: Reprodução com amplificador embutido (**NS4150B** / `GPIO 46`) e DAC **ES8311** (`0x18`), com conversão em tempo real de 24 kHz para 16 kHz.
- **Interação Natural por Toque (*Hold-to-Talk*)**:
  - Segure o dedo em qualquer ponto da tela AMOLED para falar.
  - Ao soltar, o dispositivo envia o áudio, aguarda a resposta do modelo e sintetiza a fala com animação correspondente.
- **Integração com Google Calendar (100% Standalone)**:
  - Consulta e criação de compromissos na sua agenda Google diretamente por voz.
  - Sincronização automática do relógio do sistema via NTP no fuso de Brasília (UTC-3) a cada conexão à internet.
  - O ESP32-S3 renova seu próprio token OAuth2 diretamente com `https://oauth2.googleapis.com/token` usando o bundle CA embutido do ESP-IDF.
- **Memória Persistente Autônoma (NVS Flash)**:
  - O robô possui memória de longo prazo persistente na Flash do ESP32-S3 (não se perde ao desligar ou reiniciar).
  - Ele detecta e extrai automaticamente informações importantes sobre você (como seu nome, gostos, cidade, rotina ou preferências) e as salva na NVS.
  - Em todas as conversas seguintes, essas memórias são injetadas no contexto do Gemini, permitindo que ele se lembre de você e responda de forma personalizada.
- **Avatar Robô Animado (LVGL 8.4)**:
  - Renderizado em canvas de alta velocidade via interface Quad-SPI DMA a 40 MHz no painel AMOLED **CO5300** (`480x480`).
  - Estados visuais em tempo real: **Repouso** (piscando, olhando ao redor e acenando), **Ouvindo** (ondas de áudio e olhos arregalados), **Pensando** (engrenagens girando e mão no queixo) e **Falando** (boca sincronizada e notas musicais).
  - Balão de legendas na parte inferior com transcrição do texto respondido pelo Gemini.
- **Controle Físico de Volume (+ / -)**:
  - Botões físicos superiores mapeados conforme esquemático oficial da placa:
    - **`+` (`Key3` / `GPIO 18`)**: Aumenta o volume (+10%).
    - **`-` (`Key2` / `BOOT` / `GPIO 0`)**: Diminui o volume (-10%).
  - Suporte a toque simples ou manter pressionado (*hold auto-repeat*).
  - Interrupção por hardware com *debounce* de 80 ms, barra HUD visual no topo da tela, bip de retorno sonoro e persistência na memória Flash (NVS).
- **Segurança e Proteção de Dados**:
  - Credenciais isoladas no arquivo `.env` (ignorado pelo Git).
  - Script `load_env.py` com lista estrita de variáveis (*whitelist*) e sanitização contra injeção de macros.
  - Rádio Wi-Fi mantido desligado (`WIFI_OFF`) durante repouso, ativando apenas no envio/recebimento da consulta.
  - `WiFi.persistent(false)` para não gravar a senha do Wi-Fi na memória Flash.
  - Limpeza segura de memória RAM/PSRAM (`memset` antes do `free`) após o envio e reprodução do áudio.

---

## 📌 Pinagem de Hardware (Waveshare ESP32-S3-Touch-AMOLED-2.16)

| Periférico | Barramento / Chip | Pinos no ESP32-S3 |
| :--- | :--- | :--- |
| **Display AMOLED 2.16"** | QSPI / `CO5300` (480×480) | `CS = 12`, `SCLK = 38`, `D0..D3 = 4, 5, 6, 7`, `RST = 39` |
| **Touch Capacitivo** | I2C / `CST9220` (`0x5A`) | `SDA = 15`, `SCL = 14`, `INT = 11`, `RST = 40` |
| **Microfone (ADC)** | I2C + I2S / `ES7210` (`0x40`) | `MCLK = 42`, `BCLK = 9`, `WS = 45`, `DIN = 10` |
| **Alto-Falante (DAC)** | I2C + I2S / `ES8311` (`0x18`) | `MCLK = 42`, `BCLK = 9`, `WS = 45`, `DOUT = 8`, `PA_EN = 46` |
| **Botão Volume `+`** | Tátil superior (`Key3`, `IO18`) | `GPIO 18` (pull-up R18=10K, ativo em LOW) |
| **Botão Volume `-`** | Tátil superior (`Key2`, `BOOT`) | `GPIO 0` (pull-up R9=10K, ativo em LOW) |
| **Botão Liga/Desliga** | Tátil central (`Key1`, `PWR`) | `AXP2101` (`PWRON`) / `GPIO 16` (`SYS_OUT`) |

---

## ⚙️ Como Configurar e Compilar

### 1. Pré-requisitos
- [PlatformIO](https://platformio.org/) (via extensão no VSCode ou CLI `pio`).
- Placa conectada ao PC via porta USB-C (`/dev/ttyACM0` no Linux).

### 2. Configurar o arquivo `.env`
Copie o arquivo de exemplo [`.env.sample`](.env.sample) para `.env`:

```bash
cp .env.sample .env
```

Edite o arquivo `.env` com suas credenciais:

```ini
# Rede Wi-Fi 2.4 GHz
WIFI_SSID=NomeDaSuaRede
WIFI_PASS=SuaSenhaDoWifi

# Chave gratuita da API Google Gemini (obtenha em https://aistudio.google.com/apikey)
GEMINI_API_KEY=AIzaSy...

# Modelos do Gemini
GEMINI_MODEL=gemini-3.5-flash-lite
GEMINI_TTS_MODEL=gemini-3.1-flash-tts-preview

# Integração Google Calendar (Opcional)
GOOGLE_CLIENT_ID=seu_client_id.apps.googleusercontent.com
GOOGLE_CLIENT_SECRET=seu_client_secret
GOOGLE_REFRESH_TOKEN=seu_refresh_token
```

### 3. Obter o Refresh Token do Google Calendar (Fluxo Único)

1. Crie credenciais OAuth 2.0 do tipo **Aplicativo para Computador** (Desktop App) no [Google Cloud Console](https://console.cloud.google.com/apis/credentials).
2. Ative a **Google Calendar API** no seu projeto Google Cloud.
3. Insira o `GOOGLE_CLIENT_ID` e `GOOGLE_CLIENT_SECRET` no seu arquivo `.env`.
4. Execute o script de autorização local:
   ```bash
   python3 scripts/google_auth.py
   ```
5. Faça login com sua conta Google na janela do navegador que se abrirá e autorize o escopo do Google Calendar. O script salvará o `GOOGLE_REFRESH_TOKEN` automaticamente no `.env`.

> 🔒 **Segurança**: O arquivo `.env` possui permissão restrita (`0600`) e está incluído no `.gitignore`. Suas credenciais nunca serão enviadas ao GitHub.

### 4. Compilação e Gravação

No terminal, na pasta raiz do projeto:

```bash
# Compilar e gravar no dispositivo
pio run -t upload

# Acompanhar os logs do sistema
pio device monitor
```

---

## 🎮 Como Usar

1. **Ligar o dispositivo**: Ao inicializar, o display AMOLED exibirá o avatar do robô olhando ao redor e a mensagem *"Segure a tela e fale"*.
2. **Conversar**:
   - Toque e **mantenha o dedo pressionado** na tela. O status mudará para *"Ouvindo..."*.
   - Fale a sua pergunta ou comando em português.
   - **Solte a tela**. O robô mudará para o estado *"Pensando..."* (com engrenagens animadas) enquanto consulta o Gemini.
   - Assim que o áudio for sintetizado, o robô começará a falar com animação labial sincronizada e o texto será exibido no balão de diálogo.
3. **Ajustar Volume**:
   - Pressione o botão físico **`+`** ou **`-`** na borda superior.
   - A barra de volume será exibida no topo da tela (`Volume: XX%`) e um bip de confirmação indicará a nova intensidade.
