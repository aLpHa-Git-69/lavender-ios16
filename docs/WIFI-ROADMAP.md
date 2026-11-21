# Roadmap — Driver WiFi (WCN3990 + modem MSS) — lavender

> Estado al **2026-11-21**. Progreso detallado en `work/briefs/wifi-wcn3990-brief.md`
> (agente WLAN, serie de parches `0700`–`0719`, branch `wlan-0706`). Este archivo es el
> tablero de avance: ~~tachado = verificado en el teléfono~~, `⬜ PENDIENTE` = falta.
> Convención: un archivo por función clave (`WIFI-ROADMAP.md`, y luego GPU/Audio/etc.).

## Objetivo final

Wi-Fi real en iOS 16.7.16 sobre el Redmi Note 7: scan, join WPA2, data path (DHCP,
`en0`) y visible en `wifid`/CoreWiFi/**Settings**. Hardware: WCN3990 detrás del modem
MSS (Q6) de SDM660 — el WiFi depende de arrancar el modem completo y hablarle por
GLINK/QRTR.

## Panorama

| Fase | Alcance | Estado |
|---|---|---|
| F0 | Inventario read-only (DTB, GPT, firmware, referencias) | ✅ COMPLETA |
| F1 | Arranque del modem (MBA/PIL/SCM/auth) | ✅ COMPLETA |
| F2 | GLINK/QRTR + host services + WLFW | 🔄 **EN CURSO** (bloqueo: WLFW no registra) |
| F3 | HTC/WMI/HTT + scan | ⬜ PENDIENTE |
| F4 | Assoc/WPA2 + data path + iOS (`en0`, Apple80211) | ⬜ PENDIENTE |

---

## F0 — Inventario read-only — ✅ COMPLETA

- ~~Nodo WLAN del DTB: `icnss@18800000` (WCN3990), sin SMMU de apps asignado hoy~~
- ~~SMMU ANOC2: 21 context banks, **0 SMR válidos** (`sCR0` SMMUEN=0) → sin traducción;
  GLINK/APCS no la necesitan; se re-evalúa en la etapa CE/DMA~~
- ~~Modem MSS `@4080000`: relojes, RMB, carve-outs (`modem_fw_region@8ac00000` +126 MiB,
  `pil_mba_region@94800000` 2 MiB)~~
- ~~GPT y particiones: firmware leído de la FAT16 de `modem` (LBA 786432) por
  `santoni_emmc_read`; `MBA.MBN`, `MODEM.MDT`+30 phdrs, `WLANMDSP.MBN` 3.86 MB~~
- ~~Bluetooth por SLIMbus (`slim@15240000`) — proyecto aparte, no UART HCI~~
- ~~Referencias downstream descargadas: `smem.c`/`glink_smem_native_xprt.c`
  (wahoo-4.4 y sdm660), `pil-msa.c`, `pil-q6v5.c`, `secure_buffer.c`, `gcc-sdm660.c`
  (`work/wlan/refs-gcc`)~~

## F1 — Arranque del modem (MBA/PIL/auth) — ✅ COMPLETA (parches 0700–0711)

- ~~`0700` probe de plataforma (`sweetwlan=1`)~~
- ~~`0701` relojes MSS (6 writes; bisección con `sweetmodemclk=N`)~~
- ~~`0702`–`0705` lectura de la FAT16 del `modem`: lock SDHCI (`santoni_sdhci_lock`),
  cadena FAT correcta (índice sin `fat_fat_off`), MBA.MBN cargado~~
- ~~`0706` carga de `MODEM.MDT` + segmentos (PIL order correcto: deassert restart antes
  de clocks)~~
- ~~`0707`–`0711` chunks de 2 MiB por SCM (`MEM_PROT_ASSIGN`, VMID MSS_MSA), ventanas
  IO map/unmap pareadas (`ml_io_map_unmappable`), IO alineado byte-a-byte hasta el
  límite de 8 bytes, blobs `02`–`29` (`status 00000003`, B12 21.5 MB y B17 13.7 MB por
  ventanas reutilizadas), 59 `reclaim chunk` → `MSS_MSA`~~
- ~~**AUTH COMPLETE**: `auth status 4 after 0 ms`, `auth complete, modem is running`,
  sistema vivo (~126 s). Log: `artifacts/logs/live-0711-auth-complete.txt`;
  CI `44929935655` (commit `b81de87`)~~

## F2 — GLINK/QRTR + host services + WLFW — 🔄 EN CURSO (parches 0712–0719)

### Logrado (verificado en el teléfono)

- ~~`0712`/`0713` probe SMEM v12: `$TOC` en `0x861ff000`, partición APPS/MODEM
  (`0000/0001`, off `11b000`, size `0x94000`), items 435 (SMP2P in) y 478 (desc)~~
- ~~Lección: el AP **no puede leer** particiones SMEM sin host APPS (cuelga el hilo)~~
- ~~`0714` SMP2P out: item 428 creado bajo hwlock **3** (`0x1f43000`, sfpb),
  doorbell APCS bit **14** → modem responde `slave-kernel 0x6` (READY|HANDOVER)~~
- ~~`0715`/`0716` GLINK UP: descriptor 478, TX 479 (uncached), RX 480 (**cached** —
  layout SDM660: header *después* de los datos, empaquetado desde el final), doorbell
  bit **15**, VERSION/VERSION_ACK, OPEN/OPEN_ACK, intents, QRTR HELLO, servicios
  `42:b401 servreg-notif` y `2b:1202 ssctl`~~

### ⬜ PENDIENTE (bloqueo actual)

- [ ] **WLFW (QMI servicio `0x45`) no registra.** `0717`/`0718` sirven pd-mapper pero
      el modem solo pregunta `tms/pddump_disabled -> 0 domain(s)`; nunca pide
      `tms/servreg` ni `wlan/fw`. El fix del TLV de validez (0718) no cambió el resultado.
- [ ] **`0719` en el teléfono** (shim listo, CI `44954035357`): mapea
      `tms/pddump_disabled` → `msm/modem/root_pd` y loguea el QMI crudo
      (`pd-mapper raw ... len N`) para ver los TLVs del request.
- [ ] Si el modem sigue sin preguntar por `wlan/fw`: revisar **otros host services**
      (`ssctl` completo, `tftp`) o la base real de servreg (`MODEMR.JSN`/`MODEMUW.JSN`
      dentro de `NON-HLOS.bin` → `IMAGE/`, leíble con el walker FAT16).
- [ ] **WLFW QMI `0x45`**: handshake con el PD de WLAN, entrega de `WLANMDSP.MBN`
      (3.86 MB) por chunks + SCM assigns, CE rings/SRC/DST. Criterio: `wlfw 1` /
      `WLFW ready` en el `glink summary`.
- [ ] `rmtfs`/`tftp` si el modem los reclama durante la carga del PD.

## F3 — HTC/WMI/HTT + scan — ⬜ PENDIENTE

- [ ] HTC: mailbox/rings de control hacia el PD de WLAN
- [ ] WMI: servicios + `WMI_SCAN`
- [ ] HTT: data path básico
- [ ] **Criterio de éxito:** scan con SSIDs/BSSIDs reales en el log

## F4 — Assoc/WPA2 + data path + iOS — ⬜ PENDIENTE

- [ ] VDEV create/up/start, auth/assoc, 4-way handshake (topaz `0129`/`0130` como
      referencia)
- [ ] HTT TX/RX completo, `en0` Ethernet, DHCP
- [ ] `en0` + Apple80211 para que `wifid`/CoreWiFi/**Settings** lo vean
- [ ] **Criterio de éxito:** Settings lista redes, join WPA2, Safari HTTPS

---

## Trampas conocidas (no repetir)

1. **Device memory**: nada de `memset`/`memcpy` (STNP) ni stores no alineados — usar
   `io_zero()`/`io_copy()` de 0711 (byte a byte hasta el límite de 8 bytes).
2. **eMMC**: toda lectura de firmware pasa por `santoni_sdhci_lock` + retries.
3. **`ml_io_map()`/`ml_io_unmap()` no son un par** en XNU 8796: usar
   `ml_io_map_unmappable` + `ml_io_unmap` con bounds page-aligned.
4. **SMEM**: leer solo particiones con host APPS (`0`); los items cached se empacan
   desde el final con el header después del dato (SDM660).
5. **SMMU ANOC2**: sin SMRs válidos; no asumir traducción.
6. **Verificar el shim antes de interpretar un boot**: los boot-args deciden qué
   sonda corre (`sweetwlan`/`sweetmodem` vs `lavgpu`/`lavaud`).

## Coordinación entre agentes

- Serie WiFi/modem en branch **`wlan-0706`** (merge a `main` solo cuando F2 sea
  estable, explícitamente con el usuario).
- Rangos de parches: WiFi/modem **0700–0799**, GPU **0300+**, audio **0500+**,
  diagnóstico userland/crash **0900–1000**.
- Todos los shims llevan `sweetusb=2` para el live log (`usbnet-windows.ps1 -Log`).
