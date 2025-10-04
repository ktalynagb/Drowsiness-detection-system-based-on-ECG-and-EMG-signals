SleepAlert: Sistema de Detección de Somnolencia Basado en Señales de ECG y EMG

Este proyecto presenta un **sistema de monitorización y estimulación biomédica** diseñado para detectar **niveles de somnolencia** mediante la adquisición de señales de **ECG (electrocardiograma)** y **EMG (electromiograma)**.

Cuando el sistema detecta una disminución del estado de alerta, activa una **estimulación muscular eléctrica (EMS)** como biorretroalimentación para ayudar a restablecer la vigilia.

## Descripción del Sistema
El sistema integra múltiples módulos de instrumentación biomédica:

- **Adquisición de Señales**: las señales de ECG y EMG se capturan mediante electrodos de superficie y amplificadores de instrumentación.
- **Procesamiento de Señales Analógicas y Digitales**: filtrado y detección de características para identificar indicadores de somnolencia.
- **Módulo de Estimulación**: genera un estímulo muscular basado en PWM como retroalimentación correctiva.
- **Visualización**: muestra las bioseñales en una computadora o interfaz de adquisición.

## Componentes principales
- SP32 Mini
- Amplificadores de instrumentación (INA128 / AD620)
- Filtros pasa banda activos (0,5–30 Hz)
- Circuito de estimulación basado en PWM
- Electrodos de superficie

## Objetivo
Monitorear señales fisiológicas en tiempo real, detectar somnolencia y aplicar estimulación muscular segura para mantener el estado de alerta y prevenir la fatiga.

## Autora
**Ktalyna García Benavides**
Ingeniería Biomédica

## Palabras clave
Instrumentación Biomédica · ECG · EMG · Detección de Somnolencia · Biofeedback · C++ · Estimulación Eléctrica · Python
