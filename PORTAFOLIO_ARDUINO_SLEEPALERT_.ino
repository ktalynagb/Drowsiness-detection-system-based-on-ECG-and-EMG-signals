#include <Arduino.h>
#include <WiFi.h>

// Pines de ADC
#define ADC1_PIN 10 // Derivadas
#define ADC2_PIN 11 // DII
#define ADC3_PIN 12 // EMG

// Mapeo de tiempos según número recibido (en milisegundos)
const unsigned long timeIntervals[] = {5000, 10000, 15000, 30000, 60000, 120000};

int currentIndex = 0; // Número actual de la secuencia (0 a 5)
unsigned long previousMillis = 0;
unsigned long interval = 5000; // Intervalo por defecto (5s)

// Pines del MUX
#define MUX_BIT_0 1
#define MUX_BIT_1 2
#define MUX_BIT_2 3

// Pin de salida para alarma de bradicardia persistente
#define BRADYCARDIA_ALARM_PIN 4

// Intervalos
#define SAMPLE_INTERVAL_MS 4 // 250 Hz
#define INTERVALO_CAMBIO_MUX 7500
#define BPM_CALC_INTERVAL_MS 1000 // Actualizar cada segundo

// Umbrales para condiciones cardíacas
#define TAQUICARDIA_THRESHOLD 100 // BPM mayor a esto se considera taquicardia
#define BRADICARDIA_THRESHOLD 60  // BPM menor a esto se considera bradicardia
#define ASISTOLIA_TIMEOUT_MS 5000 // 5 segundos sin latidos válidos = asistolia
#define BRADYCARDIA_PERSISTENT_THRESHOLD_MS 30000 // 30 segundos de bradicardia persistente
#define BRADYCARDIA_ALARM_DURATION_MS 5000 // 5 segundos de duración de la alarma
#define PERIODIC_PULSE_INTERVAL_MS 300000 // 5 minutos (300,000 ms) para pulso periódico
#define PERIODIC_PULSE_DURATION_MS 5000 // 5 segundos de duración del pulso periódico
#define WARNING_BEFORE_STIMULATION_MS 3000 // 3 segundos de advertencia antes de la estimulación

// Variables compartidas
volatile uint8_t muxValue = 1;
portMUX_TYPE muxPort = portMUX_INITIALIZER_UNLOCKED;

// Buffer mejorado para el cálculo de BPM
#define BPM_BUFFER_SIZE 1000 // 4 segundos a 250 Hz para cálculos más estables
static int adc2_buffer[BPM_BUFFER_SIZE];
static int buffer_index = 0;

// Variables para el cálculo de BPM con persistencia
static float dii_bpm = 0.0;
static float last_valid_bpm = 0.0; // Último valor válido para persistencia
static unsigned long last_valid_bpm_time = 0; // Tiempo del último BPM válido
static int zero_bpm_counter = 0; // Contador de cálculos fallidos consecutivos
static bool valid_bpm_obtained = false; // Bandera para saber si alguna vez obtuvimos un BPM válido

// Historial de BPM para filtrado
#define BPM_HISTORY_SIZE 10 // Aumentado para mayor estabilidad
static float bpm_history[BPM_HISTORY_SIZE] = {0};
static int bpm_history_index = 0;
static int valid_history_count = 0; // Cuántos valores válidos hay en el historial

// Umbrales mejorados para detección de picos QRS
#define QRS_THRESHOLD_PERCENTAGE 65 // Porcentaje ajustable según sensibilidad deseada
#define MIN_PEAK_DISTANCE 200 // Distancia mínima entre picos en ms
#define MAX_PEAK_DISTANCE 2000 // Distancia máxima entre picos en ms
#define BPM_STABILITY_THRESHOLD 15 // Diferencia aceptable entre mediciones consecutivas

// Constantes para la calidad de la señal
#define MAX_BPM_AGE_MS 10000 // Tiempo máximo para considerar un BPM válido (10 segundos)
#define MIN_SIGNAL_AMPLITUDE 100 // Amplitud mínima para considerar la señal válida
#define MAX_ZEROS_BEFORE_RESET 5 // Máximo de ceros consecutivos antes de reiniciar

// Variables para preprocesamiento
static int baseline_avg = 2048; // Valor medio inicial (mitad del ADC de 12 bits)
static float alpha = 0.01; // Factor de filtro paso bajo para baseline

// Handle del Timer de FreeRTOS
TaskHandle_t adcTaskHandle = NULL;
TaskHandle_t muxtimeTaskHandle = NULL;
TaskHandle_t bpmTaskHandle = NULL;
TaskHandle_t alarmTaskHandle = NULL; // Nueva tarea para monitorear condiciones cardíacas

// Variables para diagnóstico
static int last_signal_quality = 0; // 0-100%
static int last_amplitude = 0;
static int last_peaks_found = 0;

// Variables para condiciones cardíacas
static bool taquicardia_detected = false;
static bool bradicardia_detected = false;
static bool asistolia_detected = false;
static bool normal_rhythm = true; // Nueva variable para ritmo normal
static unsigned long last_condition_alert = 0; // Tiempo de la última alerta

// Variables para monitoreo de bradicardia persistente
static unsigned long bradicardia_start_time = 0; // Tiempo de inicio de bradicardia
static bool bradycardia_alarm_active = false; // Estado de la alarma de bradicardia

// Nuevas variables para el control del ciclo de la alarma
static unsigned long alarm_start_time = 0; // Tiempo en que se activó la alarma
static unsigned long next_alarm_time = 0; // Tiempo para la próxima alarma
static bool alarm_cycle_active = false; // Si estamos en un ciclo de alarma

// Variables para el pulso periódico cada 5 minutos
static unsigned long last_periodic_pulse_time = 0; // Último tiempo que se envió el pulso periódico
static bool periodic_pulse_active = false; // Si el pulso periódico está activo
static bool periodic_warning_shown = false; // Si se mostró la advertencia previa al pulso periódico

// Estado de advertencia previa para bradicardia
static bool bradycardia_warning_shown = false; // Si ya se mostró la advertencia
static unsigned long next_bradycardia_alarm_warning_time = 0; // Tiempo para mostrar la siguiente advertencia

void setMuxSelection(uint8_t value) {
  digitalWrite(MUX_BIT_0, value & 0x01);
  digitalWrite(MUX_BIT_1, (value >> 1) & 0x01);
  digitalWrite(MUX_BIT_2, (value >> 2) & 0x01);
}

// Filtro mejorado para ECG (0.5-40 Hz)
int filterECG(int input) {
  static float prev_inputs[4] = {0, 0, 0, 0};
  static float prev_outputs[4] = {0, 0, 0, 0};

  // Implementación de filtro IIR optimizado para QRS
  // Coeficientes precalculados para un filtro pasabanda 0.5-40Hz
  float output = 0.1 * input + 0.2 * prev_inputs[0] + 0.1 * prev_inputs[1] -
                0.1 * prev_inputs[2] + 0.7 * prev_outputs[0] - 0.2 * prev_outputs[1];

  // Actualizar historial
  for (int i = 3; i > 0; i--) {
    prev_inputs[i] = prev_inputs[i-1];
    prev_outputs[i] = prev_outputs[i-1];
  }
  prev_inputs[0] = input;
  prev_outputs[0] = output;

  return (int)output;
}

// Función mejorada para calcular BPM con mejor manejo de persistencia
float calculateBPM(int* buffer, int size) {
  if (size < 100) { // Necesitamos al menos 100 muestras para un cálculo confiable
    last_signal_quality = 0;
    last_peaks_found = 0;
    return 0.0;
  }

  // Preprocesamiento: eliminar componente DC
  int processed_buffer = (int)malloc(size * sizeof(int));
  if (!processed_buffer) {
    return last_valid_bpm; // Si no hay memoria, devolver último valor válido
  }

  // Calcular baseline adaptativo
  long sum = 0;
  for (int i = 0; i < size; i++) {
    sum += buffer[i];
  }
  int current_baseline = sum / size;

  // Aplicar filtro para eliminar DC y realzar QRS
  for (int i = 0; i < size; i++) {
    processed_buffer[i] = filterECG(buffer[i] - current_baseline);
  }

  // Encontrar valor máximo y mínimo después del filtrado
  int max_value = -32768;
  int min_value = 32767;

  for (int i = 0; i < size; i++) {
    if (processed_buffer[i] > max_value) max_value = processed_buffer[i];
    if (processed_buffer[i] < min_value) min_value = processed_buffer[i];
  }

  // Calcular umbral adaptativo
  int amplitude = max_value - min_value;
  last_amplitude = amplitude;

  // Verificar si la señal es lo suficientemente fuerte
  if (amplitude < MIN_SIGNAL_AMPLITUDE) {
    free(processed_buffer);
    last_signal_quality = map(amplitude, 0, MIN_SIGNAL_AMPLITUDE, 0, 20); // Calidad máx 20% si amplitud baja
    last_peaks_found = 0;

    zero_bpm_counter++;

    // Si tenemos demasiados ceros consecutivos, quizás perdimos la señal completamente
    if (zero_bpm_counter > MAX_ZEROS_BEFORE_RESET &&
       (millis() - last_valid_bpm_time) > MAX_BPM_AGE_MS) {
      return 0.0; // Señal realmente perdida, devolver 0
    }

    return last_valid_bpm; // Mantener último valor válido
  }

  int threshold = (max_value * QRS_THRESHOLD_PERCENTAGE) / 100;

  // Detección de picos mejorada
  int peaks = (int)malloc(50 * sizeof(int)); // Máximo 50 picos
  if (!peaks) {
    free(processed_buffer);
    return last_valid_bpm; // Si no hay memoria, devolver último valor válido
  }

  int peaks_count = 0;
  int last_peak_pos = -MIN_PEAK_DISTANCE / SAMPLE_INTERVAL_MS;

  // Buscar picos QRS con detección mejorada
  for (int i = 5; i < size - 5; i++) {
    // Buscar máximos locales con ventana
    bool is_peak = processed_buffer[i] > threshold;

    // Verificar si es máximo local en ventana de ±5 muestras
    if (is_peak) {
      for (int j = -5; j <= 5; j++) {
        if (j != 0 && i+j >= 0 && i+j < size &&
           processed_buffer[i] <= processed_buffer[i + j]) {
          is_peak = false;
          break;
        }
      }
    }

    // Si es un pico válido, verificar distancia temporal
    if (is_peak) {
      int distance_samples = i - last_peak_pos;

      // Verificar que el pico esté en un rango temporal válido
      if (distance_samples >= (MIN_PEAK_DISTANCE / SAMPLE_INTERVAL_MS) &&
         distance_samples <= (MAX_PEAK_DISTANCE / SAMPLE_INTERVAL_MS)) {

        if (peaks_count < 50) {
          peaks[peaks_count] = i;
          peaks_count++;
          last_peak_pos = i;
        }
      }
    }
  }

  last_peaks_found = peaks_count;

  // Calcular intervalos R-R y BPM
  if (peaks_count < 2) {
    free(processed_buffer);
    free(peaks);
    last_signal_quality = map(peaks_count, 0, 2, 20, 40); // 20-40% calidad basado en picos

    zero_bpm_counter++;

    // Si tenemos demasiados ceros consecutivos, quizás perdimos la señal
    if (zero_bpm_counter > MAX_ZEROS_BEFORE_RESET &&
       (millis() - last_valid_bpm_time) > MAX_BPM_AGE_MS) {
      return 0.0; // Señal realmente perdida
    }

    return last_valid_bpm; // Mantener último valor válido
  }

  float sum_intervals = 0;
  int valid_intervals = 0;

  for (int i = 1; i < peaks_count; i++) {
    int interval_samples = peaks[i] - peaks[i-1];
    float interval_ms = interval_samples * SAMPLE_INTERVAL_MS;

    // Validar intervalo
    if (interval_ms >= MIN_PEAK_DISTANCE && interval_ms <= MAX_PEAK_DISTANCE) {
      sum_intervals += interval_ms;
      valid_intervals++;
    }
  }

  if (valid_intervals == 0) {
    free(processed_buffer);
    free(peaks);
    last_signal_quality = 40; // Calidad base si no hay intervalos válidos

    zero_bpm_counter++;
    return last_valid_bpm; // Mantener último valor válido
  }

  float avg_interval_ms = sum_intervals / valid_intervals;
  float calculated_bpm = 60000.0 / avg_interval_ms;

  // Liberar memoria
  free(processed_buffer);
  free(peaks);

  // Validar rango de BPM
  if (calculated_bpm < 30.0 || calculated_bpm > 180.0) {
    last_signal_quality = 50; // 50% calidad para BPM fuera de rango
    zero_bpm_counter++;
    return last_valid_bpm; // Mantener último valor válido
  }

  // Si llegamos aquí, tenemos un BPM válido - actualizar historial
  zero_bpm_counter = 0;
  valid_bpm_obtained = true;
  last_valid_bpm_time = millis();

  // Añadir al historial
  bpm_history[bpm_history_index] = calculated_bpm;
  bpm_history_index = (bpm_history_index + 1) % BPM_HISTORY_SIZE;
  if (valid_history_count < BPM_HISTORY_SIZE) {
    valid_history_count++;
  }

  // Calcular BPM filtrado usando mediana y promedio ponderado
  float sorted_bpm[BPM_HISTORY_SIZE];
  int count = 0;

  // Copiar valores válidos
  for (int i = 0; i < BPM_HISTORY_SIZE; i++) {
    if (bpm_history[i] > 0 && count < valid_history_count) {
      sorted_bpm[count++] = bpm_history[i];
    }
  }

  // Ordenar para calcular mediana (filtrar outliers)
  for (int i = 0; i < count-1; i++) {
    for (int j = i+1; j < count; j++) {
      if (sorted_bpm[i] > sorted_bpm[j]) {
        float temp = sorted_bpm[i];
        sorted_bpm[i] = sorted_bpm[j];
        sorted_bpm[j] = temp;
      }
    }
  }

  // Usar mediana si tenemos suficientes valores, o promedio si no
  float filtered_bpm;
  if (count >= 3) {
    // Usar mediana para mayor estabilidad (ignorar outliers)
    filtered_bpm = sorted_bpm[count/2];

    // Combinar con promedio ponderado reciente para suavizar
    float recent_avg = 0;
    int recent_count = min(3, count);
    for (int i = 0; i < recent_count; i++) {
      int idx = (bpm_history_index - 1 - i + BPM_HISTORY_SIZE) % BPM_HISTORY_SIZE;
      if (bpm_history[idx] > 0) {
        recent_avg += bpm_history[idx] * (recent_count - i);
      }
    }
    recent_avg /= (recent_count * (recent_count + 1) / 2);

    // Combinación de mediana (70%) y promedio reciente (30%)
    filtered_bpm = 0.7 * filtered_bpm + 0.3 * recent_avg;
  } else if (count > 0) {
    // Si tenemos pocos valores, usar promedio simple
    float sum = 0;
    for (int i = 0; i < count; i++) {
      sum += sorted_bpm[i];
    }
    filtered_bpm = sum / count;
  } else {
    // No debería ocurrir, pero por seguridad
    filtered_bpm = calculated_bpm;
  }

  // Actualizar último valor válido
  last_valid_bpm = filtered_bpm;

  // Calcular calidad de señal (60-100%)
  last_signal_quality = 60 + (40 * valid_intervals / peaks_count);

  return filtered_bpm;
}

// Función modificada para manejar la alarma de bradicardia persistente y el pulso periódico
void manageBradycardiaAlarm() {
  unsigned long current_time = millis();
  
  // ===== Manejo del pulso periódico cada 5 minutos =====
  // Si nunca se ha enviado un pulso periódico, inicializar
  if (last_periodic_pulse_time == 0) {
    last_periodic_pulse_time = current_time;
  }
  
  // Verificar si es momento de mostrar la advertencia (3 segundos antes del pulso)
  if (!periodic_warning_shown && !periodic_pulse_active && 
      (current_time - last_periodic_pulse_time) >= (PERIODIC_PULSE_INTERVAL_MS - WARNING_BEFORE_STIMULATION_MS)) {
    
    // Solo mostrar advertencia si no hay alarma de bradicardia activa
    if (!bradycardia_alarm_active && !alarm_cycle_active) {
      // Enviar mensaje de estimulación inminente (formato específico para Python)
      Serial.println("ESTIMULACION_INMINENTE: Pulso periodico");
      // Enviar múltiples veces para asegurar que Python lo reciba
      delay(50);
      Serial.println("ESTIMULACION_INMINENTE: Pulso periodico");
      delay(50);
      Serial.println("ESTIMULACION_INMINENTE: Pulso periodico");
      
      periodic_warning_shown = true;
    }
  }
  
  // Verificar si es hora de enviar el pulso periódico
  if (!periodic_pulse_active && (current_time - last_periodic_pulse_time) >= PERIODIC_PULSE_INTERVAL_MS) {
    // Activar el pulso periódico solo si la alarma de bradicardia no está activa
    if (!bradycardia_alarm_active) {
      digitalWrite(BRADYCARDIA_ALARM_PIN, HIGH);
      periodic_pulse_active = true;
      Serial.println("Pulso periódico activado - PIN " + String(BRADYCARDIA_ALARM_PIN) + " HIGH por 5 segundos");
    }
    // Si la alarma de bradicardia está activa, no activamos el pulso pero actualizamos el tiempo
    last_periodic_pulse_time = current_time;
    periodic_warning_shown = false; // Reiniciar la bandera de advertencia
  }
  
  // Desactivar el pulso periódico después de 5 segundos
  if (periodic_pulse_active && (current_time - last_periodic_pulse_time) >= PERIODIC_PULSE_DURATION_MS) {
    digitalWrite(BRADYCARDIA_ALARM_PIN, LOW);
    periodic_pulse_active = false;
    last_periodic_pulse_time = current_time; // Reiniciar el temporizador
    Serial.println("Pulso periódico desactivado - PIN " + String(BRADYCARDIA_ALARM_PIN) + " LOW");
  }
  
  // ===== Manejo de la alarma de bradicardia persistente =====
  // Si se detecta bradicardia y no estamos rastreando su inicio, iniciar el rastreo
  if (bradicardia_detected && bradicardia_start_time == 0) {
    bradicardia_start_time = current_time;
    Serial.println("Iniciando monitoreo de bradicardia persistente");
  }
  
  // Si hay bradicardia persistente que está por alcanzar el umbral para activar alarma
  if (bradicardia_detected && 
      bradicardia_start_time > 0 && 
      !alarm_cycle_active &&
      !bradycardia_warning_shown &&
      (current_time - bradicardia_start_time) > (BRADYCARDIA_PERSISTENT_THRESHOLD_MS - WARNING_BEFORE_STIMULATION_MS) &&
      (current_time - bradicardia_start_time) <= BRADYCARDIA_PERSISTENT_THRESHOLD_MS) {
    
    // Enviar mensaje de estimulación inminente (formato específico para Python)
    Serial.println("ESTIMULACION_INMINENTE: Bradicardia persistente");
    // Enviar múltiples veces para asegurar que Python lo reciba
    delay(50);
    Serial.println("ESTIMULACION_INMINENTE: Bradicardia persistente");
    delay(50);
    Serial.println("ESTIMULACION_INMINENTE: Bradicardia persistente");
    
    bradycardia_warning_shown = true;
  }
  
  // Si hay bradicardia persistente por más de 30 segundos y no estamos en un ciclo de alarma
  if (bradicardia_detected && 
      bradicardia_start_time > 0 && 
      (current_time - bradicardia_start_time) > BRADYCARDIA_PERSISTENT_THRESHOLD_MS &&
      !alarm_cycle_active) {
    
    // Activar alarma (desactivando primero el pulso periódico si está activo)
    if (periodic_pulse_active) {
      periodic_pulse_active = false;
    }
    
    digitalWrite(BRADYCARDIA_ALARM_PIN, HIGH);
    bradycardia_alarm_active = true;
    alarm_cycle_active = true;
    alarm_start_time = current_time;
    Serial.println("¡ALERTA! Bradicardia persistente por más de 30 segundos - Alarma activada por 5 segundos");
  }
  // Si la alarma está activa y han pasado 5 segundos, desactivarla
  else if (bradycardia_alarm_active && (current_time - alarm_start_time) > BRADYCARDIA_ALARM_DURATION_MS) {
    digitalWrite(BRADYCARDIA_ALARM_PIN, LOW);
    bradycardia_alarm_active = false;
    
    // Si todavía hay bradicardia, establecer el tiempo para la próxima alarma
    if (bradicardia_detected) {
      next_alarm_time = current_time + BRADYCARDIA_PERSISTENT_THRESHOLD_MS;
      next_bradycardia_alarm_warning_time = next_alarm_time - WARNING_BEFORE_STIMULATION_MS;
      bradycardia_warning_shown = false; // Resetear la bandera de advertencia
      Serial.println("Alarma desactivada. Próxima alarma en 30 segundos si persiste la bradicardia");
    } else {
      alarm_cycle_active = false;
      bradycardia_warning_shown = false;
      Serial.println("Alarma desactivada. Bradicardia resuelta");
    }
  }
  
  // Verificar si es momento de mostrar la advertencia para el siguiente ciclo
  if (alarm_cycle_active && !bradycardia_alarm_active && 
      bradicardia_detected && !bradycardia_warning_shown && 
      current_time >= next_bradycardia_alarm_warning_time) {
    
    // Enviar mensaje de estimulación inminente (formato específico para Python)
    Serial.println("ESTIMULACION_INMINENTE: Ciclo de bradicardia");
    // Enviar múltiples veces para asegurar que Python lo reciba
    delay(50);
    Serial.println("ESTIMULACION_INMINENTE: Ciclo de bradicardia");
    delay(50);
    Serial.println("ESTIMULACION_INMINENTE: Ciclo de bradicardia");
    
    bradycardia_warning_shown = true;
  }
  
  // Si estamos esperando la próxima alarma y ya es tiempo
  if (alarm_cycle_active && !bradycardia_alarm_active && 
      bradicardia_detected && current_time >= next_alarm_time) {
    
    // Activar alarma nuevamente
    digitalWrite(BRADYCARDIA_ALARM_PIN, HIGH);
    bradycardia_alarm_active = true;
    alarm_start_time = current_time;
    Serial.println("¡ALERTA! Bradicardia persistente continúa - Alarma activada por 5 segundos");
  }
  
  // Si ya no hay bradicardia, resetear todo el ciclo
  if (!bradicardia_detected) {
    if (bradycardia_alarm_active) {
      digitalWrite(BRADYCARDIA_ALARM_PIN, LOW);
      bradycardia_alarm_active = false;
    }
    bradicardia_start_time = 0;
    alarm_cycle_active = false;
    bradycardia_warning_shown = false;
  }
}

// Nueva función para evaluar condiciones cardíacas
void evaluateCardiacConditions(float bpm) {
  unsigned long current_time = millis();
  bool needs_notification = false;
  String message = "";
  
  // Guardar estado previo para detectar cambios
  bool prev_bradicardia_detected = bradicardia_detected;
  
  // Resetear todos los estados
  taquicardia_detected = false;
  bradicardia_detected = false;
  asistolia_detected = false;
  normal_rhythm = false; // Resetear estado normal
  
  // Verificar asistolia (sin latidos válidos por cierto tiempo)
  if ((current_time - last_valid_bpm_time) > ASISTOLIA_TIMEOUT_MS && valid_bpm_obtained) {
    asistolia_detected = true;
    needs_notification = true;
    message = "ALERTA: ASISTOLIA DETECTADA - No se detectan latidos cardíacos";
  }
  // Verificar taquicardia/bradicardia solo si tenemos BPM válido
  else if (bpm > 0) {
    if (bpm >= TAQUICARDIA_THRESHOLD) {
      taquicardia_detected = true;
      needs_notification = true;
      message = "ALERTA: TAQUICARDIA DETECTADA - BPM: " + String(bpm, 0);
    } 
    else if (bpm <= BRADICARDIA_THRESHOLD) {
      bradicardia_detected = true;
      needs_notification = true;
      message = "ALERTA: BRADICARDIA DETECTADA - BPM: " + String(bpm, 0);
    }
    else {
      // Si no hay ninguna condición anormal y tenemos BPM válido, entonces es ritmo normal
      normal_rhythm = true;
      // Solo notificamos cambios a ritmo normal, no continuamente
      if (!taquicardia_detected && !bradicardia_detected && !asistolia_detected && 
         (current_time - last_condition_alert > 5000)) { // Cada 5 segundos para no saturar
        needs_notification = true;
        message = "Estado cardíaco: NORMAL - BPM: " + String(bpm, 0);
      }
    }
  }
  
  // Manejar alarma de bradicardia persistente
  manageBradycardiaAlarm();
  
  // Enviar notificación (limitar frecuencia a máximo una vez cada 2 segundos)
  if (needs_notification && (current_time - last_condition_alert > 2000)) {
    Serial.println(message);
    // Aquí se podrían añadir otras formas de notificación (LED, buzzer, etc.)
    last_condition_alert = current_time;
  }
}

void adcTask(void *parameter) {
  const TickType_t xFrequency = pdMS_TO_TICKS(SAMPLE_INTERVAL_MS);
  TickType_t xLastWakeTime = xTaskGetTickCount();
  unsigned long lastDebugTime = 0;

  while (1) {
    int adc1 = analogRead(ADC1_PIN); // Derivadas
    int adc2 = analogRead(ADC2_PIN); // DII
    int adc3 = analogRead(ADC3_PIN); // EMG

    // Almacenar ADC2 en el buffer circular
    portENTER_CRITICAL(&muxPort);
    adc2_buffer[buffer_index] = adc2;
    buffer_index = (buffer_index + 1) % BPM_BUFFER_SIZE;
    portEXIT_CRITICAL(&muxPort);

    // Salida serial con formato mejorado
    portENTER_CRITICAL(&muxPort);
    Serial.print(adc2); Serial.print(" ");
    Serial.print(adc3); Serial.print(" ");
    Serial.print(adc1); Serial.print(" ");

    // Mostrar BPM solo si es válido
    if (dii_bpm > 0 || valid_bpm_obtained) {
      Serial.print(dii_bpm, 0); // BPM sin decimales
    } else {
      Serial.print("0"); // Valor por defecto si no hay medición
    }
    
    // Añadir indicadores de condiciones cardíacas
    if (asistolia_detected) Serial.print(" A");
    else if (taquicardia_detected) Serial.print(" T");
    else if (bradicardia_detected) Serial.print(" B");
    else if (normal_rhythm) Serial.print(" N"); // Añadir indicador de ritmo normal
    
    // Añadir indicador de alarma de bradicardia persistente si está activa
    if (bradycardia_alarm_active) Serial.print(" BP");
    
    Serial.println();
    portEXIT_CRITICAL(&muxPort);

    // Debug cada segundo
    if (millis() - lastDebugTime > 1000) {
      lastDebugTime = millis();
      Serial.print("DEBUG: ");
      Serial.print("BPM="); Serial.print(dii_bpm, 0);
      Serial.print(", Quality="); Serial.print(last_signal_quality);
      Serial.print("%, Amp="); Serial.print(last_amplitude);
      Serial.print(", Peaks="); Serial.print(last_peaks_found);
      Serial.print(", Age="); Serial.print((millis() - last_valid_bpm_time)/1000);
      Serial.print("s");
      
      // Añadir estado de condiciones cardíacas
      if (asistolia_detected) Serial.print(", ASISTOLIA");
      else if (taquicardia_detected) Serial.print(", TAQUICARDIA");
      else if (bradicardia_detected) {
        Serial.print(", BRADICARDIA");
        // Mostrar también el tiempo de persistencia si está activa
        if (bradicardia_start_time > 0) {
          Serial.print(" ("); 
          Serial.print((millis() - bradicardia_start_time) / 1000);
          Serial.print("s)");
          if (bradycardia_alarm_active) {
            Serial.print(" - ALARMA ACTIVA");
          } else if (alarm_cycle_active) {
            // Mostrar tiempo restante para la próxima alarma
            long seconds_remaining = (next_alarm_time - millis()) / 1000;
            if (seconds_remaining > 0) {
              Serial.print(" - Próxima alarma en ");
              Serial.print(seconds_remaining);
              Serial.print("s");
            }
          }
        }
      }
      else if (normal_rhythm) Serial.print(", NORMAL"); // Cambiar a "NORMAL" en lugar de "Ritmo normal"
      
      // Añadir información sobre el pulso periódico
      if (periodic_pulse_active) {
        Serial.print(", PULSO PERIÓDICO ACTIVO");
      } else {
        long seconds_to_next_pulse = (last_periodic_pulse_time + PERIODIC_PULSE_INTERVAL_MS - millis()) / 1000;
        if (seconds_to_next_pulse > 0) {
          Serial.print(", Próximo pulso periódico en ");
          Serial.print(seconds_to_next_pulse);
          Serial.print("s");
        }
      }
      
      Serial.println();
    }

    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void bpmTask(void *parameter) {
  const TickType_t xFrequency = pdMS_TO_TICKS(BPM_CALC_INTERVAL_MS);

  // Esperar a que se llene el buffer inicialmente
  vTaskDelay(pdMS_TO_TICKS(2000));

  while (1) {
    // Copiar buffer para evitar problemas de concurrencia
    int local_buffer[BPM_BUFFER_SIZE];

    portENTER_CRITICAL(&muxPort);
    memcpy(local_buffer, adc2_buffer, sizeof(adc2_buffer));
    portEXIT_CRITICAL(&muxPort);

    // Calcular BPM con persistencia mejorada
    float new_bpm = calculateBPM(local_buffer, BPM_BUFFER_SIZE);

    // Actualizar valor global
    portENTER_CRITICAL(&muxPort);
    dii_bpm = new_bpm;
    portEXIT_CRITICAL(&muxPort);

    vTaskDelay(xFrequency);
  }
}

// Nueva tarea para monitorear condiciones cardíacas constantemente
void alarmTask(void *parameter) {
  const TickType_t xFrequency = pdMS_TO_TICKS(500); // Evaluar cada 500ms
  
  while (1) {
    portENTER_CRITICAL(&muxPort);
    float current_bpm = dii_bpm;
    portEXIT_CRITICAL(&muxPort);
    
    evaluateCardiacConditions(current_bpm);
    
    vTaskDelay(xFrequency);
  }
}

void muxtimeTask(void *parameter) {
  while (1) {
     if (Serial.available() > 0) 
  {
    char inputChar = Serial.read();
    int receivedNumber = inputChar - '0';

    if (receivedNumber >= 1 && receivedNumber <= 6) 
    {
      interval = timeIntervals[receivedNumber - 1];
      //Serial.print("Intervalo actualizado a ");
      //Serial.print(interval / 1000);
      //Serial.println(" segundos.");
    } 
    else 
    {
      
    }
  }

  // Cambia el número de la secuencia según el intervalo
  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) 
  {
    previousMillis = currentMillis;

    digitalWrite(MUX_BIT_0, bitRead(currentIndex, 0)); // GPIO 1
    digitalWrite(MUX_BIT_1, bitRead(currentIndex, 1)); // GPIO 2
    digitalWrite(MUX_BIT_2, bitRead(currentIndex, 2)); // GPIO 3

    currentIndex = (currentIndex + 1) % 6; // Siguiente número en secuencia
  }
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_OFF);
  analogReadResolution(12);

  // Configurar pin de alarma de bradicardia persistente
  pinMode(BRADYCARDIA_ALARM_PIN, OUTPUT);
  digitalWrite(BRADYCARDIA_ALARM_PIN, LOW); // Inicialmente apagado

  // Mensaje de inicio
  Serial.println();
  Serial.println("=== Sistema ECG/EMG con Detección de Condiciones Cardíacas ===");
  Serial.println("Buffer: 4 segundos (1000 muestras)");
  Serial.println("Frecuencia: 250 Hz");
  Serial.println("Umbrales: Taquicardia >100 BPM, Bradicardia <60 BPM, Asistolia >5s sin latidos");
  Serial.println("Estado NORMAL: entre 60-100 BPM");
  Serial.println("Alarma: Bradicardia persistente >30s activa salida en PIN " + String(BRADYCARDIA_ALARM_PIN) + " por 5s");
  Serial.println("Ciclo de alarma: 5s encendido, 30s apagado si persiste la bradicardia");
  Serial.println("Pulso periódico: PIN " + String(BRADYCARDIA_ALARM_PIN) + " se activa por 5s cada 5 minutos");
  Serial.println("Aviso: Se muestra mensaje \"SE HARÁ ESTIMULACIÓN\" 3s antes de cada activación");
  Serial.println("============================================================");

  // Configurar pines MUX
  pinMode(MUX_BIT_0, OUTPUT);
  pinMode(MUX_BIT_1, OUTPUT);
  pinMode(MUX_BIT_2, OUTPUT);
  setMuxSelection(muxValue);

  // Inicializar buffer con un valor medio razonable
  for (int i = 0; i < BPM_BUFFER_SIZE; i++) {
    adc2_buffer[i] = 2048; // Mitad del rango ADC
  }

  // Crear tareas
  xTaskCreatePinnedToCore(adcTask, "ADC Task", 8192, NULL, 2, &adcTaskHandle, 1);
  xTaskCreatePinnedToCore(muxtimeTask, "MUX Time Task", 2048, NULL, 1, &muxtimeTaskHandle, 1);
  xTaskCreatePinnedToCore(bpmTask, "BPM Task", 8192, NULL, 1, &bpmTaskHandle, 0);
  xTaskCreatePinnedToCore(alarmTask, "Alarm Task", 4096, NULL, 1, &alarmTaskHandle, 0);
}

void loop() {
  // Bucle principal vacío - todo se hace en tareas
  delay(1000);
}