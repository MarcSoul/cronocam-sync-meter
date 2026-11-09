# Cronocam Sync Meter

Filtro de OBS Studio para medir en directo el desfase entre dos fuentes de audio.
Pensado para comparar una entrada de mesa con el audio de referencia de una cámara.
No necesita grabación, FFmpeg, Python ni una aplicación auxiliar.

**Estado:** prototipo. El correlador pasa las pruebas con señales sintéticas. Quedan
por validar los paquetes y el funcionamiento del filtro dentro de OBS en Windows y macOS.

## Instalación

Cuando haya una versión publicada, abre **Releases** y elige el paquete de tu sistema.

| Sistema | Paquete | Instalación |
| --- | --- | --- |
| Windows x64 | `cronocam-sync-meter-…-windows-x64.exe` | Cierra OBS y ejecuta el instalador. |
| macOS Apple Silicon e Intel | `cronocam-sync-meter-…-macos-universal.zip` | Descomprime y copia el archivo `.plugin` a `~/Library/Application Support/obs-studio/plugins/`. Reinicia OBS. |

El paquete de macOS que genera este flujo es para pruebas. Antes de publicar una
versión para el público, verifica la carga con Gatekeeper y prepara la firma y
notarización de Apple. La versión mínima de OBS se documentará tras probar las
compilaciones. El flujo de CI usa las dependencias que declara la plantilla
oficial de OBS.

## Uso

1. En el mezclador de OBS, abre los filtros de la entrada que usarás como
   referencia, por ejemplo **Audio de cabina**.
2. Añade **Cronocam Sync Meter**. Selecciona **Cámara 1** como fuente
   de comparación. La cámara tiene que estar entregando audio a OBS.
3. Mantén abierta la configuración del filtro. Haz varios golpes irregulares
   que puedan oír los dos micros y espera unos segundos.

| Lectura | Interpretación |
| --- | --- |
| −300 ms | El audio de cámara llega 300 ms después que cabina. |
| +120 ms | El audio de cámara llega 120 ms antes que cabina. |
| Sin coincidencia fiable | No hay una coincidencia clara entre las señales. |

La lectura se actualiza aproximadamente una vez por segundo y usa pasos de
5 ms en un rango inferior a ±1 s. Los 5 ms son el paso del cálculo. La
precisión real depende de la señal y de la ruta de captura de OBS. El filtro
deja el audio de cabina intacto. El audio de cámara se puede mantener fuera de
las pistas de emisión y grabación desde las propiedades avanzadas de audio.
La distancia entre micrófonos y el sonido de sala también influyen en la
medición. Esta lectura corresponde a los dos audios en los puntos de captura
del plugin. Para medir imagen contra audio se usa Cronocam.

## Compilación y publicación

El workflow **Build Cronocam Sync Meter** compila el plugin nativo con la
[plantilla oficial de OBS](https://github.com/obsproject/obs-plugintemplate)
y genera los artefactos de Windows x64 y macOS universal. Para reproducir la
compilación en un equipo con las herramientas de desarrollo correspondientes:

```sh
git clone https://github.com/obsproject/obs-plugintemplate.git .obs-template
python3 packaging/prepare-template.py .obs-template 0.1.0
cd .obs-template
cmake --preset windows-x64
cmake --build --preset windows-x64 --config Release
```

En macOS, usa el preset `macos` en ambos comandos. El workflow muestra los
pasos de empaquetado. Una etiqueta como `v0.1.0` prepara un borrador en Releases
si las pruebas y las dos compilaciones terminan correctamente.

Prueba local del correlador:

```sh
g++ -std=c++17 -O2 tests/delay.cpp -o /tmp/audio-sync-test
/tmp/audio-sync-test
```

Antes de publicar, prueba dentro de OBS con una señal común en las dos fuentes
y un retardo conocido. Comprueba el signo, el cambio de cámara, la ausencia de
señal, el borrado de filtros y la reapertura de la colección de escenas. Repite
la prueba con una cámara NDI y registra la versión de OBS y la arquitectura.

## Licencia

GPL-2.0-or-later. Véase [LICENSE](LICENSE).
