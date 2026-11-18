# Celta Volante — programa del ESP32

Programa del ESP32 (WROOM-32) que lee los botones del volante y los manda
por Bluetooth a **Celta Hub** (app de Android Auto). Este repo es público
para que Celta Hub pueda bajar las actualizaciones sin clave.

- `celta_volante/celta_volante.ino`: el programa. La versión está en
  `FIRMWARE_VERSION`.
- `CAMBIOS.md`: qué cambió en cada versión.
- `.github/workflows/build.yml`: al subir un cambio a `main`, GitHub lo
  compila (Arduino CLI, placa ESP32 Dev Module, Partition Scheme
  *Minimal SPIFFS*) y publica el release `vX.Y.Z` con `celta-volante.bin`
  y `firmware.json` (versión, tamaño, huella SHA-256, notas).

## Cómo se actualiza el volante

1. **Una sola vez, por cable** (Arduino IDE): placa *ESP32 Dev Module*,
   **Herramientas → Partition Scheme → "Minimal SPIFFS (1.9MB APP with
   OTA/190KB SPIFFS)"**, y Subir. Desde la 1.3.0 el programa sabe recibir
   actualizaciones.
2. Después, todo desde el celular: Celta Hub revisa una vez por día
   `releases/latest/download/firmware.json`; si hay una versión más nueva
   que la del volante avisa con una notificación, y en ⚙ → Volante ESP32 →
   **Actualizar** la manda por Bluetooth (1 a 2 minutos, con el auto
   detenido). El ESP32 la graba en el otro espacio de memoria, verifica la
   huella SHA-256 y recién ahí se reinicia con la nueva; si algo falla,
   sigue con la anterior.
3. También se puede cargar un `.bin` guardado en el celular.

## Publicar una versión nueva

1. Cambiar el programa y subir `FIRMWARE_VERSION`.
2. Agregar la sección `## X.Y.Z` en `CAMBIOS.md`.
3. Subir a `main`. La Action compila y publica sola.

Una copia del programa vive también en el proyecto de la app
(`CarPlay_imanu/esp32/celta_volante/`), para cargarlo por cable.
