# Cambios del programa del volante

Cada versión: la sección `## X.Y.Z` (sin otro texto en esa línea). Celta
Hub muestra estas notas al ofrecer la actualización.

## 1.5.0

- Lee los comandos del volante Chevrolet: izquierdo (crucero, RES+, SET−,
  cancelar) en GPIO 34 y derecho (volumen + y −, SRC arriba, abajo y
  apretado, teléfono y silencio) en GPIO 35. 11 teclas en total.
- Volumen + y − repiten mientras se mantienen apretados.
- Las teclas apretadas a medias se ignoran (no disparan otra tecla).
- Avisa si se desconecta el comando izquierdo.
- Los 4 botones de prueba (GPIO 25, 26, 27 y 14) siguen funcionando.

## 1.4.0

- Nombre nuevo en Bluetooth: «Auto Hub Volante» (antes «Celta Volante»).
  Sigue conectándose solo, no hace falta volver a vincularlo.

## 1.3.1

- Versión de prueba de la actualización por Bluetooth (solo agrega un
  mensaje en el Monitor serie al arrancar).

## 1.3.0

- Actualización por Bluetooth desde Celta Hub (sin cable).
- Parpadeo rápido del LED mientras se actualiza.

## 1.2.0

- Informa su versión a Celta Hub.

## 1.1.0

- Toque y mantener (0,6 s) en cada botón.

## 1.0.0

- 4 botones digitales por Bluetooth.
