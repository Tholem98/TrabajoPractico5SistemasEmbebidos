# Trabajo Practico 2
Este trabajo practico se encuentra realizado en la carpeta TP2, y dentro de la subcarpeta dev. Ahí se encontrá la screenshot de la salida de GDB, el informe, y el main

# bluepill-libopencm3

Repositorio de ejemplos para la placa STM32F103C8T6 usando la biblioteca [libopencm3](https://github.com/libopencm3/libopencm3).

Está pensado como material de apoyo para Sistemas Embebidos y cubre el recorrido bare-metal del curso: GPIO, UART, timers, interrupciones externas y antirrebote.

Este repositorio no incluye ejemplos con RTOS. Esa parte vive en `bluepill-freertos`.

## Qué aporta libopencm3

`libopencm3` es una biblioteca de bajo nivel para microcontroladores ARM Cortex-M. Proporciona acceso a periféricos mediante funciones C, con una capa más liviana que HAL y sin asistentes de configuración.

En este contexto sirve para:

- trabajar con periféricos reales sin escribir todos los registros a mano,
- mantener control fino del hardware,
- aprender un modelo de programación embebida más cercano al micro que las bibliotecas de alto nivel.

A diferencia de un proyecto basado en CMSIS puro, acá startup, vectores y drivers básicos vienen resueltos por la propia biblioteca.

## Clonado y compilación

El repositorio incluye `libopencm3` como submódulo. Para clonar correctamente:

```bash
git clone --recurse-submodules https://github.com/LeonardoAmet/bluepill-libopencm3.git
cd bluepill-libopencm3
```

Compilar la biblioteca una vez:

```bash
cd libopencm3
make
```

Luego, por ejemplo:

```bash
cd ../labs/01_blink_gpio
make
make flash
```

## Estructura del proyecto

- `labs/`: ejemplos agrupados por tema.
- `common/`: archivos compartidos, como `linker.ld`.
- `libopencm3/`: submódulo con la biblioteca original.

La separación pedagógica entre repos queda así:

- `bluepill-libopencm3`: recorrido bare-metal con libopencm3.
- `bluepill-freertos`: continuación con multitarea y sincronización usando FreeRTOS.

Cada lab tiene su propio `Makefile`, pero comparten:

- un `linker.ld` adaptado a STM32F103C8T6,
- vector de interrupciones y startup provistos por libopencm3.

## Dónde seguir leyendo

- documentación oficial: https://libopencm3.org
- código fuente: https://github.com/libopencm3/libopencm3
- ejemplos externos: `libopencm3-examples`

Archivos útiles dentro de la biblioteca:

- `libopencm3/lib/cm3/vector.c`
- `libopencm3/lib/cm3/vector_nvic.c`

## Alcance en la cursada

La idea de este repositorio es acompañar las primeras etapas del trabajo embebido:

- inicialización del micro,
- GPIO,
- UART,
- timers,
- EXTI,
- técnicas de antirrebote.

Es un repositorio orientado a práctica guiada, no a generar firmware final complejo.

## Primera prueba sugerida

`labs/01_blink_gpio`

Ese ejemplo permite verificar rápidamente:

- compilación correcta,
- flashing,
- configuración de reloj,
- uso básico de GPIOC/PC13.

## Repositorios relacionados

- [bluepill-cmsis-drivers](https://github.com/LeonardoAmet/bluepill-cmsis-drivers)
- [bluepill-freertos](https://github.com/LeonardoAmet/bluepill-freertos)
