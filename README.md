# Claude Buddy — WT32-SC01

Un compañero físico para Claude Code: **Clawd** (la mascota pixel de Claude) vive en una
pantalla WT32-SC01 de 3.5" y muestra en tiempo real la actividad de tus sesiones y los
límites de uso del Plan (ventana de 5 h, semanal y por modelo — lo mismo que `/usage`).

```
┌──────────────────────────────────────────────┐
│ C L A U D E  B U D D Y            [MAX]  ●   │
│                       ┌────────────────────┐ │
│    ┌──────────┐       │ 5h        13%  ▓▓░ │ │
│    │  ▐▌  ▐▌  │       ├────────────────────┤ │
│    │          │       │ Semana    17%  ▓▓░ │ │
│    │    ──    │       ├────────────────────┤ │
│    └──────────┘       │ Fable     31%  ▓▓░ │ │
│     trabajando...     └────────────────────┘ │
│ hoy: 384k tokens      actividad: ahora       │
└──────────────────────────────────────────────┘
```

Clawd reacciona: parpadea y respira en idle, rebota mientras hay una sesión activa,
duerme (`z z z`) tras 30 min sin actividad, suda cerca del límite (≥85 %) y se apaga
gris con la boca larga cuando un límite llega al 100 %.

## Arquitectura

```
~/.claude (credenciales + actividad)
        │
        ▼
companion/server.js  (Node, sin dependencias)  →  http://<PC>:8787/status
        │  · consulta el endpoint OAuth de usage de Anthropic cada 60 s
        │  · detecta sesiones activas por mtime de ~/.claude/projects/**.jsonl
        ▼
firmware (ESP32 + LVGL)  →  hace polling HTTP cada 15 s por WiFi
```

## 1. Companion (en la PC donde corre Claude Code)

Requiere Node 18+. Sin `npm install`:

```bash
node companion/server.js
```

Probalo: `http://localhost:8787/status`. Para que arranque solo con Windows:

```powershell
schtasks /Create /TN "ClaudeBuddyCompanion" /TR "node D:\Projects\Claude_WT32_screen_buddy\companion\server.js" /SC ONLOGON /RL LIMITED
```

Notas:
- Usa el token OAuth que Claude Code ya guarda en `~/.claude/.credentials.json`
  (solo lectura; nunca sale de tu PC — la pantalla solo recibe porcentajes).
- Si el token expira, corré cualquier comando `claude` y se refresca solo.

## 2. Firmware (WT32-SC01)

```bash
cd firmware
cp include/config.example.h include/config.h   # editar WiFi y la IP de tu PC
pio run -e wt32-sc01 -t upload                 # o: python -m platformio run ...
```

- Placa original (ESP32, micro-USB): entorno `wt32-sc01`.
- Placa **Plus** (ESP32-S3, USB-C): entorno `wt32-sc01-plus`.

`config.h` está gitignoreado — ahí van tu SSID, contraseña y la URL del companion.

## Pomodoro

Deslizá hacia la izquierda para pasar del buddy al **timer pomodoro** (y a la derecha
para volver). Ciclo tradicional: 25' foco / 5' descanso, con descanso largo de 15'
cada 4 pomodoros (los 4 puntos indican la posición en el set).

- Tap en el círculo: arrancar / pausar / reanudar.
- ⏭ salta la sesión (un foco salteado no suma al contador) · ⟳ reinicia la sesión.
- El descanso arranca solo al terminar el foco; el próximo foco espera tu tap.
- Al terminar una sesión la pantalla parpadea y salta a la vista del pomodoro.
- "ciclos hoy" se persiste en NVS y se resetea a medianoche (fecha del companion).
- El timer sigue corriendo aunque estés mirando a Clawd.

## Estados de Clawd

| Estado | Cuándo | Animación |
|---|---|---|
| durmiendo | > 30 min sin actividad | ojos cerrados, `z z z` |
| listo | actividad reciente, sin sesión activa | respira, parpadea |
| trabajando | archivo de sesión modificado < 5 min | rebota, ojos se mueven |
| sudando | algún límite ≥ 85 % | gota azul |
| límite | algún límite = 100 % | cuerpo gris, boca larga |
| sin WiFi / companion | error de conexión | atenuado + punto rojo |
