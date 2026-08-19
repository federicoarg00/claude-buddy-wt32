# Claude Buddy — WT32-SC01

Un compañero físico para Claude Code: **Claudito** (la mascota pixel de Claude) vive en una
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

Claudito reacciona: parpadea y respira en idle, rebota mientras hay una sesión activa,
duerme (`z z z`) tras 30 min sin actividad, suda cerca del límite (≥85 %) y se apaga
gris con la boca larga cuando un límite llega al 100 %.

## Arquitectura (híbrida)

```
        EN CASA                                EN CUALQUIER LADO
~/.claude (credenciales + actividad)     cuenta de Claude (sesión OAuth propia
        │                                del buddy, provisionada una vez)
        ▼                                          ▲ HTTPS + roots CA pineados
companion/server.js → http://<PC>:8787/status      │
        │  · usage de Anthropic cada 120 s         │ api.anthropic.com/api/oauth/usage
        │  · actividad por mtime de transcripts    │ cada 120 s, backoff ante 429
        ▼                                          │
firmware (ESP32 + LVGL) ── intenta companion ──────┘ si no lo encuentra: modo directo
```

El buddy intenta el companion primero (datos ricos: tokens del día, sesiones).
Si no lo alcanza — estás de viaje, la PC apagada — pasa solo a **modo directo**:
consulta tu cuenta de Claude directamente (ícono de antena en vez de casita).
Multi-WiFi: agregá redes conocidas (trabajo, hotspot del celu) en `config.h`
vía `WIFI_EXTRA_NETWORKS`. En modo directo la actividad se infiere del avance
del % de la ventana de 5 h (detecta uso desde cualquier dispositivo, incluso
el móvil); "hoy: N tokens" solo está disponible en modo casa.

### Provisioning del modo directo (una sola vez)

```bash
node provision/provision.js
```

Abre el navegador para autorizar al buddy en tu cuenta (flujo OAuth + PKCE de
Claude, sesión independiente — no toca el login de Claude Code), y le manda los
tokens a la pantalla por la LAN (la IP aparece en pantalla hasta provisionar).
Los tokens viven en la flash del dispositivo (NVS) y se auto-renuevan; si
perdés el buddy, revocá la sesión desde claude.ai.

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
- El timer sigue corriendo aunque estés mirando a Claudito.

## Calendario de pomodoros (tercera pantalla)

Desde el pomodoro, deslizá otra vez a la izquierda: calendario de los **últimos
30 días** (columnas L-D, hoy con borde) con código de colores por ciclos
completados — **verde >10 · amarillo 5-10 · rojo 1-4 · gris sin datos** — y
panel de estadísticas (total, promedio/día, mejor día, hoy). El historial se
acumula en la flash desde el día que instalaste esta versión.

## Estados de Claudito

| Estado | Cuándo | Animación |
|---|---|---|
| durmiendo | > 30 min sin actividad | ojos cerrados, `z z z` |
| listo | actividad reciente, sin sesión activa | respira, parpadea |
| trabajando | archivo de sesión modificado < 5 min | rebota, ojos se mueven |
| sudando | algún límite ≥ 85 % | gota azul |
| límite | algún límite = 100 % | cuerpo gris, boca larga |
| sin WiFi / companion | error de conexión | atenuado + punto rojo |
