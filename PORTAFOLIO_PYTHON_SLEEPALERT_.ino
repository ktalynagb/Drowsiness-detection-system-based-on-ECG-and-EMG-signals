import serial
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.widgets import Button
import tkinter as tk
from tkinter import messagebox, simpledialog
from matplotlib.backends.backend_pdf import PdfPages
import datetime
import os

# === SEGUNDA CONEXIÓN: LECTURA PARA GRAFICAR ===
ser = serial.Serial('COM6', 115200)

# Configuración de tiempo basada en el código Arduino
muestreo = 1000  # Frecuencia de muestreo en Hz (1 ms = 1000 Hz)
flag=0

# === DATOS PARA VISUALIZACIÓN (ventana deslizante) ===
signal_1 = []  # Para visualización
signal_2 = []  # Para visualización
signal_3 = []  # Para visualización
tiempo = []    # Para visualización

# === DATOS COMPLETOS PARA ANÁLISIS (toda la sesión) ===
signal_1_complete = []  # Almacena TODOS los datos de DII
signal_2_complete = []  # Almacena TODOS los datos de EMG
signal_3_complete = []  # Almacena TODOS los datos de Derivadas
tiempo_complete = []    # Almacena TODOS los tiempos

# === VARIABLES PARA ELECTROESTIMULACIÓN ===
stimulation_log = []  # Lista para almacenar los instantes de estimulación
stimulation_times = []  # Lista para almacenar los tiempos de estimulación
ext_flag = 0  # Flag para la función de detección
stimulation_counter = 0  # Contador global de estimulaciones

tiempo_inicial = None  # Para calcular el tiempo relativo
dii_frequency = 0.0  # Variable para almacenar la frecuencia DII

# Variables para condiciones cardíacas
cardiac_condition = "NORMAL"  # Inicializar con estado normal

# Variable para controlar el mensaje de estimulación
stimulation_warning_active = False
stimulation_warning_time = 0
stimulation_warning_duration = 3000  # 3 segundos en ms
stimulation_message = ""

# Variable global para datos del paciente (se inicializa vacía)
patient_data = {}

# === VARIABLES PARA ANÁLISIS DE TENDENCIA (DATOS COMPLETOS) ===
trend_time_log = []  # Lista para almacenar timestamps COMPLETOS
trend_output_log = []  # Lista para almacenar las salidas (1-6) COMPLETAS
last_trend_time = 0  # Para controlar frecuencia de registro
trend_interval = 0.1  # Registrar cada 100ms

# Umbrales según la tabla proporcionada
bpm_threshold_low = 60    # Menor a 60 bpm = bajo
bmp_threshold_high = 100  # Mayor a 100 bpm = alto
emg_threshold_low = 0.1   # Menor a 0.1V = bajo (aproximadamente 0V)
emg_threshold_normal = 0.7 # Alrededor de 0.9V = normal
emg_threshold_high = 1.5  # Mayor a 1.5V = alto

# === FUNCIÓN DE DETECCIÓN DE ELECTROESTIMULACIÓN ===
def deteccion_estimulacion(stimulation_signal, flag):
    """
    Función para detectar y contar las electroestimulaciones
    """
    if flag == 0:
        lectura_estimulo = np.zeros(len(stimulation_signal))
        instante_estimulo = np.zeros(len(stimulation_signal))
        contador = 0
    
    flag = 1
    contador = 0
    lectura_estimulo = np.zeros(len(stimulation_signal))
    instante_estimulo = np.zeros(len(stimulation_signal))
    
    for idx in range(len(stimulation_signal)):
        if stimulation_signal[idx] == 1:
            lectura_estimulo[idx] = 1.0
            instante_estimulo[idx] = idx
            contador += 1
        else:
            lectura_estimulo[idx] = 0.0
            instante_estimulo[idx] = idx
    
    instante_estimulo = instante_estimulo * (1/1000)  # Convertir a segundos
    
    return instante_estimulo, lectura_estimulo, contador, flag

def determine_current_output(dii_freq, emg_value, cardiac_condition):
    """
    Determina la salida actual basada en BPM y EMG según la tabla:
    1. Somnolencia: BPM bajo + EMG bajo
    2. Bradicardia: BPM bajo + EMG normal
    3. EMG_bajo: BPM normal + EMG bajo
    4. Vigilia: BPM normal + EMG normal
    5. Taquicardia: BPM alto + EMG normal
    6. Despierto: BPM alto + EMG alto
    """
    
    # Clasificar BPM
    if 0 < dii_freq < bpm_threshold_low:
        bmp_status = "bajo"
    elif dii_freq > bmp_threshold_high:
        bmp_status = "alto"
    else:
        bmp_status = "normal"
    
    # Clasificar EMG
    if emg_value < emg_threshold_low:
        emg_status = "bajo"
    elif emg_value > emg_threshold_high:
        emg_status = "alto"
    else:
        emg_status = "normal"
    
    # Determinar salida según combinación BPM + EMG
    if bmp_status == "bajo" and emg_status == "bajo":
        return 1  # Somnolencia
    elif bmp_status == "bajo" and emg_status == "normal":
        return 2  # Bradicardia
    elif bmp_status == "normal" and emg_status == "bajo":
        return 3  # EMG_bajo
    elif bmp_status == "normal" and emg_status == "normal":
        return 4  # Vigilia
    elif bmp_status == "alto" and emg_status == "normal":
        return 5  # Taquicardia
    elif bmp_status == "alto" and emg_status == "alto":
        return 6  # Despierto
    else:
        # Casos no contemplados en la tabla, usar lógica por defecto
        if bmp_status == "bajo":
            return 2 if emg_status != "bajo" else 1
        elif bmp_status == "alto":
            return 6 if emg_status == "alto" else 5
        else:
            return 4  # Default: Vigilia

def update_trend_log():
    global trend_time_log, trend_output_log, last_trend_time
    
    # Usar los datos completos en lugar de la ventana de visualización
    current_time = len(tiempo_complete) / muestreo if tiempo_complete else 0
    
    # Solo registrar si ha pasado el intervalo mínimo
    if current_time - last_trend_time >= trend_interval:
        if len(signal_1_complete) > 0 and len(signal_2_complete) > 0:
            # Obtener valores actuales de los datos COMPLETOS
            current_dii_freq = dii_frequency
            current_emg = signal_2_complete[-1] if signal_2_complete else 0
            
            # Determinar salida actual
            current_output = determine_current_output(current_dii_freq, current_emg, cardiac_condition)
            
            # Registrar en el log COMPLETO
            trend_time_log.append(current_time)
            trend_output_log.append(current_output)
            last_trend_time = current_time

def create_stimulation_analysis_page(pdf):
    """
    Crear página de análisis de electroestimulación - Conteo de activaciones PIN4
    """
    global stimulation_times, stimulation_counter
    
    # Crear figura para análisis de estimulación
    fig_stim, (ax_main, ax_stats) = plt.subplots(2, 1, figsize=(10, 11), 
                                                gridspec_kw={'height_ratios': [3, 1]})
    
    fig_stim.suptitle('Registro de activaciones de electroestimulación', fontsize=16, fontweight='bold')
    
    # === GRÁFICO PRINCIPAL DE ESTIMULACIÓN ===
    if len(stimulation_times) > 0:
        # Crear señal de estimulación para graficar
        stimulation_signal = np.ones(len(stimulation_times))  # Pulsos HIGH del PIN4
        
        markerline, stemlines, baseline = ax_main.stem(stimulation_times, stimulation_signal, 'r-', markerfmt='ro', basefmt='k-', linefmt='r-')
        stemlines.set_linewidth(2)  # Aplicar linewidth a las líneas stem
        markerline.set_markersize(8)  # Hacer los marcadores más grandes
        
        ax_main.set_ylabel('Estimulación', fontsize=12)
        ax_main.set_title(f'Número de estimulaciones durante el tiempo (Total: {stimulation_counter})', fontsize=14)
        ax_main.grid(True, alpha=0.3)
        ax_main.set_ylim(-0.1, 1.5)
        
        # Configurar etiquetas del eje Y
        ax_main.set_yticks([0, 1])
        ax_main.set_yticklabels(['LOW', 'HIGH'])
        
        # Añadir líneas verticales para mejor visualización
        for i, stim_time in enumerate(stimulation_times):
            ax_main.axvline(x=stim_time, color='red', linestyle='--', alpha=0.5)
            
        # Añadir numeración a los primeros pulsos para claridad
        for i, stim_time in enumerate(stimulation_times[:15]):  # Máximo 15 para evitar saturación
            ax_main.text(stim_time, 1.1, f'{i+1}', ha='center', va='bottom', 
                        fontsize=8, color='red', fontweight='bold')
            
    else:
        ax_main.text(0.5, 0.5, 'No se enviaron señales de estimulación', 
                    ha='center', va='center', transform=ax_main.transAxes, fontsize=12)
        ax_main.set_ylabel('Estimulaciones', fontsize=12)
        ax_main.set_yticks([0, 1])
        ax_main.set_yticklabels(['LOW', 'HIGH'])
    
    ax_main.set_xlabel('Tiempo (s)', fontsize=12)
    
    # === TABLA DE ESTADÍSTICAS ===
    ax_stats.axis('off')  # Ocultar ejes para la tabla
    
    # Calcular estadísticas de estimulación
    total_session_time = tiempo_complete[-1] if tiempo_complete else 0
    
    if len(stimulation_times) > 0:
        time_between_stimulations = np.diff(stimulation_times) if len(stimulation_times) > 1 else [0]
        avg_interval = np.mean(time_between_stimulations) if len(time_between_stimulations) > 0 else 0
        min_interval = np.min(time_between_stimulations) if len(time_between_stimulations) > 0 else 0
        max_interval = np.max(time_between_stimulations) if len(time_between_stimulations) > 0 else 0
        stimulation_rate = stimulation_counter / (total_session_time / 60) if total_session_time > 0 else 0  # por minuto
        
        # Calcular densidad de estimulaciones
        if total_session_time > 0:
            stimulation_density = stimulation_counter / total_session_time  # estimulaciones por segundo
        else:
            stimulation_density = 0
    else:
        avg_interval = min_interval = max_interval = stimulation_rate = stimulation_density = 0
    
    stats_data = [
        ['Total estimulaciones', f"{stimulation_counter}"],
        ['Duración de sesión', f"{total_session_time:.2f} s ({total_session_time/60:.2f} min)"],
    
    ]
    
    # Crear tabla
    headers = ['Parámetro', 'Valor']
    table = ax_stats.table(cellText=stats_data,
                          colLabels=headers,
                          cellLoc='center',
                          loc='center',
                          bbox=[0, 0, 1, 1])
    
    table.auto_set_font_size(False)
    table.set_fontsize(11)
    table.scale(1, 2)
    
    # Estilizar tabla
    for i in range(len(headers)):
        table[(0, i)].set_facecolor('#FF4444')
        table[(0, i)].set_text_props(weight='bold', color='white')
    
    # Colorear filas alternadas
    for i in range(1, len(stats_data) + 1):
        for j in range(len(headers)):
            if i % 2 == 0:
                table[(i, j)].set_facecolor('#f5f5f5')
    
    # === INFORMACIÓN ADICIONAL ===
    if stimulation_counter > 0:
        first_stimulation = min(stimulation_times)
        last_stimulation = max(stimulation_times)
        duration_with_stim = last_stimulation - first_stimulation
        info_text = f"""
REGISTRO DE ACTIVACIONES PIN4:
• Primera activación: {first_stimulation:.2f} s
• Última activación: {last_stimulation:.2f} s
• Sistema automático funcionando correctamente
        """
    else:
        info_text = """
REGISTRO DE ACTIVACIONES PIN4:
• No se detectaron electroestimulaciones durante la sesión
• El sistema automático no determinó necesidad de estimulación
        """
    
    fig_stim.text(0.02, 0.02, info_text, fontsize=10, 
                 bbox=dict(boxstyle="round", facecolor='#ffcccc', alpha=0.8),
                 verticalalignment='bottom')
    
    # Ajustar layout
    plt.tight_layout()
    fig_stim.subplots_adjust(top=0.93, bottom=0.15)
    
    # Guardar en PDF
    pdf.savefig(fig_stim)
    plt.close(fig_stim)

def create_trend_analysis_page(pdf):
    """
    Crear página de análisis de tendencia usando TODOS los datos de la sesión
    """
    global trend_time_log, trend_output_log
    
    if len(trend_time_log) == 0:
        return  # No hay datos para analizar
    
    # Crear figura para análisis de tendencia
    fig_trend, (ax_main, ax_stats) = plt.subplots(2, 1, figsize=(10, 11), 
                                                  gridspec_kw={'height_ratios': [3, 1]})
    
    fig_trend.suptitle('Análisis de tendencia de estado de somnoliencia', fontsize=16, fontweight='bold')
    
    # === GRÁFICO PRINCIPAL DE TENDENCIA ===
    ax_main.plot(trend_time_log, trend_output_log, 'b-', linewidth=2, marker='o', markersize=3)
    ax_main.set_xlabel('Tiempo (s)', fontsize=12)
    ax_main.set_ylabel('Salida del Sistema', fontsize=12)
    ax_main.set_title('Evolución del estado de somnolencia en el tiempo', fontsize=14)
    ax_main.grid(True, alpha=0.3)
    
    # Configurar etiquetas del eje Y
    ax_main.set_yticks([1, 2, 3, 4, 5, 6])
    ax_main.set_yticklabels(['1-Somnolencia', '2-Bradicardia', '3-EMG_bajo', 
                           '4-Vigilia', '5-Taquicardia', '6-Despierto'], fontsize=10)
    ax_main.set_ylim(0.5, 6.5)
    
    # Añadir líneas horizontales para mejor visualización
    for y in range(1, 7):
        ax_main.axhline(y=y, color='gray', linestyle='--', alpha=0.3)
    
    # === TABLA DE ESTADÍSTICAS ===
    ax_stats.axis('off')  # Ocultar ejes para la tabla
    
    # Calcular estadísticas
    output_names = ['Somnolencia', 'Bradicardia', 'EMG_bajo', 'Vigilia', 'Taquicardia', 'Despierto']
    total_time = trend_time_log[-1] - trend_time_log[0] if len(trend_time_log) > 1 else 0
    
    stats_data = []
    for output_num in range(1, 7):
        count = trend_output_log.count(output_num)
        percentage = (count / len(trend_output_log)) * 100 if len(trend_output_log) > 0 else 0
        time_in_state = (count * trend_interval)
        
        stats_data.append([
            f"{output_num}",
            output_names[output_num-1],
            f"{count}",
            f"{percentage:.1f}%",
            f"{time_in_state:.1f}s"
        ])
    
    # Crear tabla
    headers = ['Salida', 'Estado', 'Muestras', 'Porcentaje', 'Tiempo']
    table = ax_stats.table(cellText=stats_data,
                          colLabels=headers,
                          cellLoc='center',
                          loc='center',
                          bbox=[0, 0, 1, 1])
    
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1, 2)
    
    # Estilizar tabla
    for i in range(len(headers)):
        table[(0, i)].set_facecolor('#4CAF50')
        table[(0, i)].set_text_props(weight='bold', color='white')
    
    # Colorear filas alternadas
    for i in range(1, len(stats_data) + 1):
        for j in range(len(headers)):
            if i % 2 == 0:
                table[(i, j)].set_facecolor('#f0f0f0')
    
    # === INFORMACIÓN ADICIONAL ===
    info_text = f"""
RESUMEN DEL ANÁLISIS DE LA SESIÓN COMPLETA:
• Tiempo total de monitoreo: {total_time:.2f} segundos ({total_time/60:.2f} minutos)
• Muestras registradas: {len(trend_output_log)}
• Tiempo de muestreo: {trend_interval:.1f} 
• Estado más frecuente: {output_names[max(set(trend_output_log), key=trend_output_log.count)-1] if trend_output_log else 'N/A'}
    """
    
    fig_trend.text(0.02, 0.02, info_text, fontsize=10, 
                   bbox=dict(boxstyle="round", facecolor='lightblue', alpha=0.8),
                   verticalalignment='bottom')
    
    # Ajustar layout
    plt.tight_layout()
    fig_trend.subplots_adjust(top=0.93, bottom=0.15)
    
    # Guardar en PDF
    pdf.savefig(fig_trend)
    plt.close(fig_trend)

# Configurar gráfico
fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(10, 8))
fig.suptitle('Visualización de Señales', fontsize=16)

# Configurar títulos y etiquetas de ejes
ax1.set_title('Señal DII')
ax2.set_title('Señal EMG')
ax3.set_title('Señal Derivadas')

# Añadir etiquetas para los ejes
ax1.set_ylabel('Voltaje [V]')
ax2.set_ylabel('Voltaje [V]')
ax3.set_ylabel('Voltaje [V]')
ax3.set_xlabel('Tiempo [s]')  # Cambiado a segundos

# Crear líneas para los gráficos
line1, = ax1.plot([], [], color='b')
line2, = ax2.plot([], [], color='g')
line3, = ax3.plot([], [], color='r')

# Configurar límites iniciales - CAMBIO A 1 SEGUNDO
ventana = 1000  # Ahora almacenamos 1000 muestras = 1 segundo a 1000 Hz
ventana_tiempo = ventana / muestreo  # Convertir ventana a segundos (ahora 1.0 segundos)
ax1.set_xlim(0, ventana_tiempo)
ax2.set_xlim(0, ventana_tiempo)
ax3.set_xlim(0, ventana_tiempo)
ax1.set_ylim(0, 0.2)
ax2.set_ylim(0, 0.2)
ax3.set_ylim(0, 0.2)

# Habilitar interacción y zoom
plt.tight_layout()
fig.subplots_adjust(top=0.9, hspace=0.3, bottom=0.15)

# Variable para controlar la pausa
paused = False

# Crear botones para controlar la visualización
ax_pause = plt.axes([0.8, 0.01, 0.1, 0.04])
button_pause = Button(ax_pause, 'Pausar')

# Añadir botones para ajustar el zoom de los ejes Y
ax_zoom_in_y = plt.axes([0.65, 0.01, 0.1, 0.04])
button_zoom_in_y = Button(ax_zoom_in_y, 'Zoom Y+')

ax_zoom_out_y = plt.axes([0.5, 0.01, 0.1, 0.04])
button_zoom_out_y = Button(ax_zoom_out_y, 'Zoom Y-')

# Añadir botones para ajustar el zoom de los ejes X
ax_zoom_in_x = plt.axes([0.35, 0.01, 0.1, 0.04])
button_zoom_in_x = Button(ax_zoom_in_x, 'Zoom X+')

ax_zoom_out_x = plt.axes([0.2, 0.01, 0.1, 0.04])
button_zoom_out_x = Button(ax_zoom_out_x, 'Zoom X-')

# Añadir un botón para guardar en PDF
ax_save_pdf = plt.axes([0.05, 0.06, 0.1, 0.04])
button_save_pdf = Button(ax_save_pdf, 'Guardar PDF')

# Mover texto de frecuencia a la esquina superior derecha
frequency_text = fig.text(0.95, 0.95, 'Frecuencia DII: 0.00 Hz', ha='right', fontsize=12, 
                         bbox=dict(boxstyle="round", facecolor='white', alpha=0.8))

# Añadir texto para mostrar alertas cardíacas en la esquina superior izquierda
cardiac_alert_text = fig.text(0.05, 0.95, 'NORMAL', ha='left', fontsize=12, fontweight='bold',
                            bbox=dict(boxstyle="round", facecolor='lightgreen', alpha=0.8))

# Añadir texto para mostrar contador de estimulaciones
stimulation_counter_text = fig.text(0.05, 0.85, 'Estimulaciones: 0', ha='left', fontsize=10, fontweight='bold',
                                  bbox=dict(boxstyle="round", facecolor='lightcoral', alpha=0.8))

# Añadir texto para el mensaje de estimulación (inicialmente invisible)
stimulation_alert_text = fig.text(0.5, 0.5, 'SE HARÁ ESTIMULACIÓN', ha='center', va='center',
                                fontsize=36, fontweight='bold', color='white', visible=False,
                                bbox=dict(boxstyle="round,pad=1", facecolor='red', edgecolor='black', linewidth=3, alpha=0.9))

# Función para recopilar datos del paciente
def collect_patient_data():
    """Recopila los datos del paciente usando diálogos de entrada"""
    global patient_data
    
    print("\n=== RECOPILANDO DATOS DEL PACIENTE ===")
    
    try:
        patient_name = input("Nombre del paciente: ").strip()
        if not patient_name:
            patient_name = "Sin especificar"
            
        patient_age = input("Edad: ").strip()
        if not patient_age:
            patient_age = "Sin especificar"
            
        patient_id = input("ID/Documento: ").strip()
        if not patient_id:
            patient_id = "Sin especificar"
        
        # Crear diccionario con datos del paciente 
        patient_data = {
            'name': patient_name,
            'age': patient_age,
            'id': patient_id,
            'date': datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        }
        
        print(f"\n DATOS REGISTRADOS")
        return True
        
    except KeyboardInterrupt:
        print("\nOperación cancelada por el usuario.")
        return False
    except Exception as e:
        print(f"Error al recopilar datos: {e}")
        return False

# Funciones para los botones
def toggle_pause(event):
    global paused
    paused = not paused
    button_pause.label.set_text('Reanudar' if paused else 'Pausar')
    plt.draw()

def zoom_in_y(event):
    current_ylim1 = ax1.get_ylim()
    current_ylim2 = ax2.get_ylim()
    current_ylim3 = ax3.get_ylim()
    # Reducir el rango del eje Y para hacer zoom in
    ax1.set_ylim(current_ylim1[0], current_ylim1[1] * 0.5)
    ax2.set_ylim(current_ylim2[0], current_ylim2[1] * 0.5)
    ax3.set_ylim(current_ylim3[0], current_ylim3[1] * 0.5)
    plt.draw()

def zoom_out_y(event):
    current_ylim1 = ax1.get_ylim()
    current_ylim2 = ax2.get_ylim()
    current_ylim3 = ax3.get_ylim()
    # Aumentar el rango del eje Y para hacer zoom out
    ax1.set_ylim(current_ylim1[0], current_ylim1[1] * 2)
    ax2.set_ylim(current_ylim2[0], current_ylim2[1] * 2)
    ax3.set_ylim(current_ylim3[0], current_ylim3[1] * 2)
    plt.draw()

def zoom_in_x(event):
    # Obtener los límites actuales
    current_xlim1 = ax1.get_xlim()
    current_xlim2 = ax2.get_xlim()
    current_xlim3 = ax3.get_xlim()
    
    # Calcular el centro del rango visible
    center1 = (current_xlim1[0] + current_xlim1[1]) / 2
    center2 = (current_xlim2[0] + current_xlim2[1]) / 2
    center3 = (current_xlim3[0] + current_xlim3[1]) / 2
    
    # Calcular la mitad del nuevo rango (la mitad del rango actual)
    range1 = (current_xlim1[1] - current_xlim1[0]) / 2
    range2 = (current_xlim2[1] - current_xlim2[0]) / 2
    range3 = (current_xlim3[1] - current_xlim3[0]) / 2
    
    # Establecer nuevos límites, asegurando que el mínimo no sea negativo
    ax1.set_xlim(max(0, center1 - range1/2), center1 + range1/2)
    ax2.set_xlim(max(0, center2 - range2/2), center2 + range2/2)
    ax3.set_xlim(max(0, center3 - range3/2), center3 + range3/2)
    
    plt.draw()

def zoom_out_x(event):
    # Obtener los límites actuales
    current_xlim1 = ax1.get_xlim()
    current_xlim2 = ax2.get_xlim()
    current_xlim3 = ax3.get_xlim()
    
    # Calcular el centro del rango visible
    center1 = (current_xlim1[0] + current_xlim1[1]) / 2
    center2 = (current_xlim2[0] + current_xlim2[1]) / 2
    center3 = (current_xlim3[0] + current_xlim3[1]) / 2
    
    # Calcular el nuevo rango (1.5 veces el rango actual)
    range1 = (current_xlim1[1] - current_xlim1[0]) * 1.5
    range2 = (current_xlim2[1] - current_xlim2[0]) * 1.5
    range3 = (current_xlim3[1] - current_xlim3[0]) * 1.5
    
    # Establecer nuevos límites, asegurando que el mínimo no sea negativo
    ax1.set_xlim(max(0, center1 - range1/2), center1 + range1/2)
    ax2.set_xlim(max(0, center2 - range2/2), center2 + range2/2)
    ax3.set_xlim(max(0, center3 - range3/2), center3 + range3/2)
    
    plt.draw()

# Añadir un botón para restablecer la vista
ax_reset = plt.axes([0.05, 0.01, 0.1, 0.04])
button_reset = Button(ax_reset, 'Reset')

def reset_view(event):
    global signal_1, tiempo
    
    # Restablecer los límites a la ventana de tiempo
    ax1.set_xlim(0, ventana_tiempo)
    ax2.set_xlim(0, ventana_tiempo)
    ax3.set_xlim(0, ventana_tiempo)
    
    # Restablecer los límites de Y
    ax1.set_ylim(0, 0.2)
    ax2.set_ylim(0, 0.2)
    ax3.set_ylim(0, 0.2)
    
    plt.draw()

# Función para mostrar el menú de opciones de PDF en terminal
def show_pdf_options():
    """Muestra un menú en terminal para seleccionar las opciones del PDF"""
    print("\n=== OPCIONES DE PDF ===")
    print("1. Todas las gráficas en una sola página")
    print("2. Dos graficas por página (DII+EMG en página 1, Derivadas en página 2)")
    print("3. Tres graficas por página")
    print("0. Cancelar")
    
    while True:
        try:
            choice = input("Seleccione una opción (1-3, 0 para cancelar): ").strip()
            if choice in ['1', '2', '3']:
                return choice
            elif choice == '0':
                return None
            else:
                print("Opción inválida. Ingrese un número del 1 al 3, o 0 para cancelar.")
        except KeyboardInterrupt:
            print("\nOperación cancelada por el usuario.")
            return None
        except Exception as e:
            print(f"Error: {e}. Intente nuevamente.")
            continue

# Función mejorada para guardar gráficas en PDF con opciones - USAR VENTANA ACTUAL
def save_pdf(event=None):
    global signal_1, signal_2, signal_3, tiempo, paused, button_pause, dii_frequency, cardiac_condition, patient_data
    
    # Verificar si hay datos para guardar
    if len(signal_1) == 0:
        print("Advertencia: No hay datos para guardar.")
        return
    
    # Recopilar datos del paciente antes de guardar
    if not collect_patient_data():
        print("Guardado cancelado - No se pudieron recopilar los datos del paciente.")
        return
    
    # Mostrar opciones de PDF
    pdf_option = show_pdf_options()
    
    if pdf_option is None:  # Usuario canceló
        return
    
    # Pausar la adquisición mientras se guarda
    old_paused_state = paused
    paused = True
    
    # Crear nombre de archivo con datos del paciente y fecha/hora
    now = datetime.datetime.now()
    safe_patient_name = "".join(c for c in patient_data['name'] if c.isalnum() or c in (' ', '-', '_')).rstrip()
    if not safe_patient_name or safe_patient_name == "Sin especificar":
        safe_patient_name = "Paciente"
    filename = f"Señales_{safe_patient_name}{now.strftime('%Y-%m-%d%H-%M-%S')}.pdf"
    
    # Crear un directorio para guardar los archivos si no existe
    save_dir = "Señales_Guardadas"
    if not os.path.exists(save_dir):
        os.makedirs(save_dir)
    
    filepath = os.path.join(save_dir, filename)
    
    try:
        # Obtener los límites actuales de la ventana
        current_xlim = ax1.get_xlim()
        current_ylim1 = ax1.get_ylim()
        current_ylim2 = ax2.get_ylim()
        current_ylim3 = ax3.get_ylim()
        
        # Crear un PDF con las gráficas según la opción seleccionada
        with PdfPages(filepath) as pdf:
            # Añadir metadatos al PDF
            d = pdf.infodict()
            d['Title'] = f'Registro de Señales - {patient_data["name"]}'
            d['Author'] = 'Sistema de Monitoreo Cardíaco'
            d['Subject'] = f'Señales DII, EMG y Derivadas - Paciente: {patient_data["name"]}'
            d['Keywords'] = f'Paciente: {patient_data["name"]}, Edad: {patient_data["age"]}, ID: {patient_data["id"]}, Frecuencia DII: {dii_frequency:.2f} Hz, Condición: {cardiac_condition}, Estimulaciones: {stimulation_counter}'
            d['CreationDate'] = datetime.datetime.today()
            
            if pdf_option == "1":  # Todas las gráficas en una sola página + análisis
                create_single_page_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
            
            elif pdf_option == "2":  # 2 derivadas por página + análisis
                create_two_per_page_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
            
            elif pdf_option == "3":  # 3 derivadas por página + análisis
                create_three_separate_pages_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
        
        # Mostrar mensaje de éxito
        print(f"Gráficas guardadas como: {filepath}")
        
    except Exception as e:
        # Mostrar mensaje de error
        print(f"Error al guardar: {str(e)}")
    
    # Restaurar estado de pausa
    paused = old_paused_state
    button_pause.label.set_text('Reanudar' if paused else 'Pausar')
    plt.draw()

def create_single_page_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con todas las gráficas en una sola página + páginas de análisis"""
    # Crear la página original
    create_single_page_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
    # Añadir página de análisis de tendencia
    create_trend_analysis_page(pdf)
    # Añadir página de análisis de electroestimulación
    create_stimulation_analysis_page(pdf)

def create_two_per_page_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con 2 derivadas por página + páginas de análisis"""
    # Crear las páginas originales
    create_two_per_page_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
    # Añadir página de análisis de tendencia
    create_trend_analysis_page(pdf)
    # Añadir página de análisis de electroestimulación
    create_stimulation_analysis_page(pdf)

def create_three_separate_pages_pdf_with_analysis(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con 3 derivadas en páginas separadas + páginas de análisis"""
    # Crear las páginas originales
    create_three_separate_pages_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3)
    # Añadir página de análisis de tendencia
    create_trend_analysis_page(pdf)
    # Añadir página de análisis de electroestimulación
    create_stimulation_analysis_page(pdf)

def create_single_page_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con todas las gráficas en una sola página - USAR VENTANA ACTUAL"""
    fig_pdf, (ax1_pdf, ax2_pdf, ax3_pdf) = plt.subplots(3, 1, figsize=(10, 8))
    fig_pdf.suptitle('Visualización de Señales - Todas las Derivadas', fontsize=16)
    
    # Configurar las gráficas
    ax1_pdf.set_title('Señal DII')
    ax2_pdf.set_title('Señal EMG')
    ax3_pdf.set_title('Señal Derivadas')
    
    ax1_pdf.set_ylabel('Voltaje (V)')
    ax2_pdf.set_ylabel('Voltaje (V)')
    ax3_pdf.set_ylabel('Voltaje (V)')
    ax3_pdf.set_xlabel('Tiempo (s)')
    
    # Añadir información en la parte superior - INCLUIR info del paciente ya que DII está presente
    add_pdf_metadata(fig_pdf, include_patient_info=True, include_frequency=True)
    
    # Aplicar límites y datos USANDO VENTANA ACTUAL
    set_axis_limits_and_data(ax1_pdf, ax2_pdf, ax3_pdf, current_xlim, 
                           current_ylim1, current_ylim2, current_ylim3)
    
    plt.tight_layout()
    fig_pdf.subplots_adjust(top=0.85, hspace=0.3, bottom=0.15)
    pdf.savefig(fig_pdf)
    plt.close(fig_pdf)

def create_two_per_page_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con 2 derivadas por página - USAR VENTANA ACTUAL"""
    # Página 1: DII y EMG
    fig_pdf1, (ax1_pdf, ax2_pdf) = plt.subplots(2, 1, figsize=(10, 8))
    fig_pdf1.suptitle('Señales DII y EMG', fontsize=16)
    
    ax1_pdf.set_title('Señal DII')
    ax2_pdf.set_title('Señal EMG')
    ax1_pdf.set_ylabel('Voltaje (V)')
    ax2_pdf.set_ylabel('Voltaje (V)')
    ax2_pdf.set_xlabel('Tiempo (s)')
    
    # INCLUIR info del paciente Y frecuencia ya que DII está presente
    add_pdf_metadata(fig_pdf1, include_patient_info=True, include_frequency=True)
    
    # Configurar límites y datos para DII y EMG USANDO VENTANA ACTUAL
    ax1_pdf.set_xlim(current_xlim)
    ax2_pdf.set_xlim(current_xlim)
    ax1_pdf.set_ylim(current_ylim1)
    ax2_pdf.set_ylim(current_ylim2)
    
    ax1_pdf.plot(tiempo, signal_1, color='b')
    ax2_pdf.plot(tiempo, signal_2, color='g')
    
    plt.tight_layout()
    fig_pdf1.subplots_adjust(top=0.85, hspace=0.3, bottom=0.15)
    pdf.savefig(fig_pdf1)
    plt.close(fig_pdf1)
    
    # Página 2: Derivadas USANDO VENTANA ACTUAL
    fig_pdf2, ax3_pdf = plt.subplots(1, 1, figsize=(10, 8))
    fig_pdf2.suptitle('Señal Derivadas', fontsize=16)
    
    ax3_pdf.set_title('Señal Derivadas')
    ax3_pdf.set_ylabel('Voltaje (V)')
    ax3_pdf.set_xlabel('Tiempo (s)')
    
    # NO incluir info del paciente NI frecuencia en esta página
    add_pdf_metadata(fig_pdf2, include_patient_info=False, include_frequency=False)
    
    ax3_pdf.set_xlim(current_xlim)
    ax3_pdf.set_ylim(current_ylim3)
    ax3_pdf.plot(tiempo, signal_3, color='r')
    
    plt.tight_layout()
    fig_pdf2.subplots_adjust(top=0.85, bottom=0.15)
    pdf.savefig(fig_pdf2)
    plt.close(fig_pdf2)

def create_three_separate_pages_pdf(pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Crear PDF con 3 derivadas en páginas separadas - USAR VENTANA ACTUAL"""
    signals = [
        (signal_1, 'Señal DII', current_ylim1, 'b', True, True),   # TRUE para incluir info del paciente Y frecuencia
        (signal_2, 'Señal EMG', current_ylim2, 'g', False, False), # FALSE para no incluir info NI frecuencia
        (signal_3, 'Señal Derivadas', current_ylim3, 'r', False, False) # FALSE para no incluir info NI frecuencia
    ]
    
    for signal, title, ylim, color, include_patient_info, include_frequency in signals:
        fig_pdf, ax_pdf = plt.subplots(1, 1, figsize=(10, 8))
        fig_pdf.suptitle(title, fontsize=16)
        
        ax_pdf.set_title(title)
        ax_pdf.set_ylabel('Voltaje (V)')
        ax_pdf.set_xlabel('Tiempo (s)')
        
        # Solo incluir info del paciente Y frecuencia en la página de DII
        add_pdf_metadata(fig_pdf, include_patient_info=include_patient_info, include_frequency=include_frequency)
        
        # USAR VENTANA ACTUAL
        ax_pdf.set_xlim(current_xlim)
        ax_pdf.set_ylim(ylim)
        ax_pdf.plot(tiempo, signal, color=color)
        
        plt.tight_layout()
        fig_pdf.subplots_adjust(top=0.85, bottom=0.15)
        pdf.savefig(fig_pdf)
        plt.close(fig_pdf)

def add_pdf_metadata(fig, include_patient_info=False, include_frequency=False):
    """Añadir metadatos comunes a todas las páginas del PDF"""
    
    # Solo mostrar información del paciente si se especifica
    if include_patient_info:
        # Información del paciente en la esquina superior izquierda
        patient_info = f"Paciente: {patient_data['name']}\nEdad: {patient_data['age']} | ID: {patient_data['id']}\nFecha: {patient_data['date']}\nEstimulaciones aplicadas: {stimulation_counter}"
        fig.text(0.02, 0.98, patient_info, ha='left', va='top', fontsize=9, 
                bbox=dict(boxstyle="round", facecolor='lightblue', alpha=0.8))
        
        # Condición cardíaca solo en páginas con info del paciente
        if cardiac_condition:
            fig.text(0.98, 0.92, cardiac_condition, ha='right', va='top', fontsize=12, fontweight='bold',
                   bbox=dict(boxstyle="round", facecolor=get_condition_color(cardiac_condition), alpha=0.8))
    
    # Frecuencia en la esquina superior derecha (SOLO si se especifica)
    if include_frequency:
        fig.text(0.98, 0.98, f'Frecuencia DII: {dii_frequency:.2f} Hz', ha='right', va='top', fontsize=12, 
                bbox=dict(boxstyle="round", facecolor='white', alpha=0.8))

def set_axis_limits_and_data(ax1_pdf, ax2_pdf, ax3_pdf, current_xlim, current_ylim1, current_ylim2, current_ylim3):
    """Configurar límites y datos para los ejes en PDF de una página - USAR VENTANA ACTUAL"""
    # USAR DATOS DE VISUALIZACIÓN (VENTANA ACTUAL) EN LUGAR DE DATOS COMPLETOS
    ax1_pdf.set_xlim(current_xlim)
    ax2_pdf.set_xlim(current_xlim)
    ax3_pdf.set_xlim(current_xlim)
    ax1_pdf.set_ylim(current_ylim1)
    ax2_pdf.set_ylim(current_ylim2)
    ax3_pdf.set_ylim(current_ylim3)
    
    # Crear las gráficas con los datos ACTUALES DE LA VENTANA
    ax1_pdf.plot(tiempo, signal_1, color='b')
    ax2_pdf.plot(tiempo, signal_2, color='g')
    ax3_pdf.plot(tiempo, signal_3, color='r')

# Función para obtener color según la condición cardíaca
def get_condition_color(condition):
    if condition == "TAQUICARDIA":
        return 'red'
    elif condition == "BRADICARDIA":
        return 'yellow'
    elif condition == "ASISTOLIA":
        return 'darkred'
    elif condition == "NORMAL":
        return 'lightgreen'
    else:
        return 'white'

# Función para mostrar/ocultar el mensaje de estimulación
def update_stimulation_warning():
    global stimulation_warning_active, stimulation_warning_time, stimulation_alert_text, stimulation_message
    
    current_time = datetime.datetime.now().timestamp() * 1000  # Tiempo actual en milisegundos
    
    # Si la advertencia está activa, verificar si debemos ocultarla
    if stimulation_warning_active:
        if current_time - stimulation_warning_time >= stimulation_warning_duration:
            stimulation_alert_text.set_visible(False)
            stimulation_warning_active = False
            plt.draw()  # Forzar redibujado
    
    # Si debe estar visible, asegurar que muestra el texto correcto
    if stimulation_warning_active and not stimulation_alert_text.get_visible():
        stimulation_alert_text.set_visible(True)
        plt.draw()  # Forzar redibujado

# Asignar funciones a los botones
button_pause.on_clicked(toggle_pause)
button_zoom_in_y.on_clicked(zoom_in_y)
button_zoom_out_y.on_clicked(zoom_out_y)
button_zoom_in_x.on_clicked(zoom_in_x)
button_zoom_out_x.on_clicked(zoom_out_x)
button_reset.on_clicked(reset_view)
button_save_pdf.on_clicked(save_pdf)

def update_plot(frame):
    global paused, signal_1, signal_2, signal_3, tiempo, tiempo_inicial, dii_frequency
    global frequency_text, cardiac_condition, cardiac_alert_text
    global stimulation_warning_active, stimulation_warning_time, stimulation_message, stimulation_alert_text
    global stimulation_counter, stimulation_times, stimulation_counter_text
    # NUEVAS VARIABLES GLOBALES PARA DATOS COMPLETOS
    global signal_1_complete, signal_2_complete, signal_3_complete, tiempo_complete
    
    # Inicializar nuevo_dato fuera del bloque if
    nuevo_dato = False
    
    # Actualizar el estado del mensaje de estimulación
    update_stimulation_warning()
    
    if not paused:
        # Actualizar datos desde serial
        while ser.in_waiting > 0:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                
                # Verificar si es un mensaje de estimulación inminente
                if "ESTIMULACION_INMINENTE" in line:
                    # Activar mensaje de advertencia
                    stimulation_warning_active = True
                    stimulation_warning_time = datetime.datetime.now().timestamp() * 1000
                    
                    # Extraer información adicional del mensaje
                    if ":" in line:
                        stimulation_message = line.split("ESTIMULACION_INMINENTE:")[1].strip()
                    else:
                        stimulation_message = ""
                    
                    # Actualizar el texto de advertencia
                    if stimulation_message:
                        stimulation_alert_text.set_text(f"SE HARÁ ESTIMULACIÓN\n{stimulation_message}")
                    else:
                        stimulation_alert_text.set_text("SE HARÁ ESTIMULACIÓN")
                    
                    # Hacer visible el texto de advertencia
                    stimulation_alert_text.set_visible(True)
                    plt.draw()  # Forzar actualización inmediata
                    continue  # Saltar al siguiente mensaje sin procesar datos
                
                # Procesamiento normal de datos
                data = line.split()
                
                # Verificar si es un mensaje de estimulación realizada (PIN4 HIGH)
                if ("ALARMA ACTIVADA" in line or "PULSO PERIÓDICO ACTIVADO" in line or 
    "PIN4_HIGH" in line or "HIGH" in line or "¡ALERTA! Bradicardia persistente por más de 30 segundos - Alarma activada por 5 segundos" in line ):
                    # Incrementar contador de estimulaciones
                    stimulation_counter += 1
                    
                    # Registrar el tiempo de la estimulación
                    current_time = len(tiempo_complete) / muestreo if tiempo_complete else 0
                    stimulation_times.append(current_time)
                    
                    # Actualizar el texto del contador en la interfaz
                    stimulation_counter_text.set_text(f'Estimulaciones: {stimulation_counter}')
                    
                    plt.draw()  # Actualizar display
                    continue  # Saltar al siguiente mensaje sin procesar datos
                
                # Verificar formato con indicadores de condiciones cardíacas
                if len(data) >= 4 and all(c.isdigit() or c == '.' or c == '-' for c in data[0]) and all(c.isdigit() or c == '.' or c == '-' for c in data[1]) and all(c.isdigit() or c == '.' or c == '-' for c in data[2]) and all(c.isdigit() or c == '.' or c == '-' for c in data[3]):
                    # Convertir datos
                    val1 = int(data[0])*(3.3 / 4095)  # DII
                    val2 = int(data[1])*(3.3 / 4095)  # EMG
                    val3 = int(data[2])*(3.3 / 4095)  # Derivadas
                    
                    # AÑADIR A DATOS COMPLETOS (SIEMPRE)
                    signal_1_complete.append(val1)
                    signal_2_complete.append(val2)
                    signal_3_complete.append(val3)
                    
                    # AÑADIR A DATOS DE VISUALIZACIÓN (ventana deslizante)
                    signal_1.append(val1)
                    signal_2.append(val2)
                    signal_3.append(val3)
                    
                    # Actualizar frecuencia
                    dii_frequency = float(data[3])
                    frequency_text.set_text(f'Frecuencia DII: {dii_frequency:.2f} Hz')
                    
                    # Comprobar si hay indicador de condición cardíaca (A, T, B, N)
                    prev_condition = cardiac_condition
                    if len(data) > 4:
                        if "A" in data[4]:  # Asistolia
                            cardiac_condition = "ASISTOLIA"
                        elif "T" in data[4]:  # Taquicardia
                            cardiac_condition = "TAQUICARDIA"
                        elif "B" in data[4]:  # Bradicardia
                            cardiac_condition = "BRADICARDIA"
                        elif "N" in data[4]:  # Normal
                            cardiac_condition = "NORMAL"
                    
                    # Actualizar el texto de alerta si cambia la condición
                    if cardiac_condition != prev_condition:
                        cardiac_alert_text.set_text(cardiac_condition)
                        cardiac_alert_text.set_bbox(dict(boxstyle="round", 
                                                     facecolor=get_condition_color(cardiac_condition), 
                                                     alpha=0.8))
                    
                    # Inicializar tiempo inicial si es la primera muestra
                    if tiempo_inicial is None and len(signal_1_complete) == 1:
                        tiempo_inicial = 0
                        
                    # AÑADIR TIEMPO PARA DATOS COMPLETOS
                    if tiempo_inicial is not None:
                        tiempo_complete.append((len(signal_1_complete) - 1) / muestreo)
                        # AÑADIR TIEMPO PARA VISUALIZACIÓN
                        tiempo.append((len(signal_1) - 1) / muestreo)
                    
                    nuevo_dato = True
                    
            except Exception as e:
                print(f"Error procesando línea: {e}")
                continue

        # Mantener solo las últimas N muestras PARA VISUALIZACIÓN (NO para datos completos)
        if len(signal_1) > ventana:
            signal_1[:] = signal_1[-ventana:]
            signal_2[:] = signal_2[-ventana:]
            signal_3[:] = signal_3[-ventana:]
            tiempo[:] = tiempo[-ventana:]
            
            # Ajustar el tiempo para que comience desde 0 EN LA VISUALIZACIÓN
            tiempo_min = tiempo[0]
            tiempo[:] = [t - tiempo_min for t in tiempo]
    
    # REGISTRO DE TENDENCIA USANDO DATOS COMPLETOS
    if not paused and nuevo_dato:
        update_trend_log()
    
    # Actualizar los datos en las gráficas DE VISUALIZACIÓN
    if tiempo:  # Solo si hay datos
        line1.set_data(tiempo, signal_1)
        line2.set_data(tiempo, signal_2)
        line3.set_data(tiempo, signal_3)
        
        # Auto-ajuste de los límites si no estamos haciendo zoom
        for ax in [ax1, ax2, ax3]:
            current_xlim = ax.get_xlim()
            # Si estamos cerca de los límites por defecto o si hay nuevos datos
            if abs(current_xlim[1] - ventana_tiempo) < ventana_tiempo/10 and nuevo_dato:
                ax.set_xlim(0, ventana_tiempo)

    return line1, line2, line3

# Menú simple en terminal como estaba originalmente
num = input("Ingresa un número del 1 al 6 (o 'q' para salir): ")

if num and num.lower() != 'q':
    # Enviar configuración al Arduino
    ser.write((num + '\n').encode())
    
    # Iniciar la animación
    ani = FuncAnimation(fig, update_plot, blit=False, interval=20, cache_frame_data=False)
    plt.show()
else:
    print("Sistema terminado.")

# Cerrar el puerto al final
ser.close()