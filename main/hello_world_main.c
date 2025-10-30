/*
 * Trabalho M2 - Sistemas em Tempo Real
 * Temática 1: Drone
 * Autores: Daniel Henrique da Silva e Milene Emmel Rovedder
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOSConfig.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Includes de Rede
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define TAG "DRONE" // TAG para logs

#define LED_GPIO GPIO_NUM_2 // LED 
// Mapeamento dos touch pads para eventos do sistema
#define TOUCH_A TOUCH_PAD_NUM3 // (GPIO 15) -> Perturbação/Sobrecarga
#define TOUCH_B TOUCH_PAD_NUM4 // (GPIO 13) -> Controle de rota / Navegação
#define TOUCH_C TOUCH_PAD_NUM7 // (GPIO 27) -> Telemetria
#define TOUCH_D TOUCH_PAD_NUM9 // (GPIO 32) -> Fail-safe / Emergência

#define DEBOUNCE_DELAY_MS 200 // Constante para o tempo de debounce em milissegundos

#define FUS_T_MS 5    // Período da FUS_IMU em milissegundos
#define STK      3072 // Tamanho da stack para as tarefas

// Deadlines para cada tarefa em milissegundos
#define FUS_IMU_DEADLINE_MS  5
#define CTRL_ATT_DEADLINE_MS 5
#define NAV_PLAN_DEADLINE_MS 20
#define FS_TASK_DEADLINE_MS  10
#define MONITOR_DEADLINE_MS  500
#define TIME_DEADLINE_MS     1000

// Carga de trabalho simulada (WCET) em microssegundos
#define WCET_FUS_IMU_US      1000
#define WCET_CTRL_ATT_US     800
#define WCET_NAV_PLAN_US     3500
#define WCET_FS_TASK_US      900
#define WCET_TELEMETRY_US    500
#define WCET_PERTURBATION_US 2000 // Carga extra para a perturbação

// Rede WiFi
#define WIFI_SSID "DIRCE801"
#define WIFI_PASS "11042024"

// IP e Porta do PC (Servidor TCP/UDP)
#define PC_SERVER_IP   "192.168.0.12" // IP do PC (Servidor)
#define PC_SERVER_PORT 6010           // Porta do PC (Servidor)

// Política de escalonamento
// #define POLICY_RATE_MONOTONIC
// #define POLICY_DEADLINE_MONOTONIC
#define POLICY_CUSTOM_CRITICALITY

// Modo de MONITOR_TASK (TCP ou UDP)
// Linha comentada: Cliente UDP // Linha descomentada: Cliente TCP
#define MONITOR_MODE_TCP

// Prioridades com base na política escolhida
#ifdef POLICY_RATE_MONOTONIC
    #define PRIO_FUS_IMU      5
    #define PRIO_FS_TASK      4
    #define PRIO_PERTURBATION 4
    #define PRIO_NAV_PLAN     3
    #define PRIO_CTRL_ATT     2
    #define PRIO_MONITOR      1 
    #define PRIO_TIME         1 
    #define POLICY_NAME "Rate Monotonic (RM)"
#endif
#ifdef POLICY_DEADLINE_MONOTONIC
    #define PRIO_FUS_IMU      5
    #define PRIO_CTRL_ATT     5
    #define PRIO_FS_TASK      4
    #define PRIO_PERTURBATION 4
    #define PRIO_NAV_PLAN     3
    #define PRIO_MONITOR      2 
    #define PRIO_TIME         1
    #define POLICY_NAME "Deadline Monotonic (DM)"
#endif
#ifdef POLICY_CUSTOM_CRITICALITY
    #define PRIO_FS_TASK      6 // Emergência tem a maior prioridade
    #define PRIO_FUS_IMU      5
    #define PRIO_PERTURBATION 5
    #define PRIO_CTRL_ATT     4
    #define PRIO_NAV_PLAN     3
    #define PRIO_MONITOR      2 // Rede - não crítica
    #define PRIO_TIME         1 // SNTP menor prioridade
    #define POLICY_NAME "Custom Criticality"
#endif

static TaskHandle_t hFUS = NULL, hCTRL = NULL, hNAV = NULL, hFS = NULL; // Handles para referenciar as tarefas
typedef enum { EV_NAV = 1, EV_TEL = 2 } nav_evt_t; // Eventos para NAV_PLAN (Navegação e Telemetria)
static QueueHandle_t qNav = NULL; // Fila para NAV_PLAN - Escolhida por ser flexível e poder enviar dados (o tipo de evento).
static SemaphoreHandle_t semFS = NULL; // Semáforo para o Fail-Safe - Escolhido por ser o mecanismo mais rápido para sinalizar de uma ISR
static SemaphoreHandle_t semPerturbation = NULL; // Semáforo para a tarefa de perturbação.
static SemaphoreHandle_t xStateMutex = NULL; // Mutex para simular recurso compartilhado (estado do drone)

// Struct para simular o estado (orientação) do drone
typedef struct {
    float roll, pitch, yaw;
} state_t;
static state_t g_state = {0};

// Timestamps para cálculo de latência
volatile int64_t isr_event_timestamp = 0, isr_fs_timestamp = 0;

// Métricas de WCRT para o relatório
volatile int64_t fus_imu_wcrt_exec = 0;
volatile int64_t ctrl_att_wcrt_exec = 0;

// Contadores de deadline misses para o relatório
static int fus_imu_misses = 0, ctrl_att_misses = 0, nav_plan_misses = 0, fs_task_misses = 0; 

// Estrutura para rastrear quantas vezes cada tarefa é bloqueada ao tentar obter o xStateMutex.
typedef struct {
    int fus_imu_blocks;      // Bloqueios sofridos pela FUS_IMU
    int ctrl_att_blocks;     // Bloqueios sofridos pela CTRL_ATT
    int nav_plan_blocks;     // Bloqueios sofridos pela NAV_PLAN
    int time_blocks;         // Bloqueios sofridos pela TIME_TASK (no xTimeMutex)
    int perturbation_blocks; // Bloqueios sofridos pela TASK_PERTURBATION
} block_stats_t;
static block_stats_t g_blocks = {0}; // Inicializa todos os contadores com zero

// Variáveis para o cálculo total de (m,k)-firm sobre todos os eventos nav
#define K_WINDOW_SIZE 10 // Janela de 10 execuções
#define NAV_M_MIN      9 // m = 9 (Requisito mínimo para (9, 10)-firm)  
volatile int mk_firm_buffer[K_WINDOW_SIZE] = {0}; // Buffer de 10 posições (0: miss, 1: hit)
volatile int mk_firm_index = 0;    // Índice do buffer circular (0 a 9)
volatile int mk_firm_failures = 0; // Contador de falhas (m < 9)
volatile uint64_t nav_total_runs    = 0; // Total de execuções de NAV_PLAN (k total)
volatile uint64_t nav_total_success = 0; // Total de sucessos de NAV_PLAN (m total)

// Variáveis para as estatísticas do relatório automático
volatile uint64_t nav_event_count = 0;
volatile uint64_t nav_latency_sum = 0;
volatile int64_t  nav_latency_max = 0;
volatile uint64_t fs_event_count  = 0;
volatile uint64_t fs_latency_sum  = 0;
volatile int64_t  fs_latency_max  = 0;

// Variáveis para RTT
volatile uint64_t rtt_count = 0;
volatile uint64_t rtt_sum   = 0;
volatile int64_t  rtt_max   = 0;

#define HWM_BUFFER_SIZE 1000 // Salva as últimas 1000 latências
static int64_t fs_latencies[HWM_BUFFER_SIZE]  = {0};
static int64_t nav_latencies[HWM_BUFFER_SIZE] = {0};

// Variável para controlar o tempo do debounce
volatile int64_t last_isr_time = 0;

// Controle do LED - desligamento seguro
volatile int64_t led_off_time = 0;

// SNTP
static SemaphoreHandle_t xTimeMutex = NULL; // Protege as vars de tempo
static time_t g_now = 0;                    // Tempo atual (epoch)
static struct tm g_timeinfo = {0};          // Tempo atual (formatado)
static int sntp_sync_cycles = 0;            // Contador de sincronizações
static sntp_sync_status_t last_sync_status = SNTP_SYNC_STATUS_RESET; // Status para detectar mudança

// Função para simular carga de trabalho (WCET) de forma previsível
static inline void cpu_tight_loop_us(uint32_t us) {
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < us) { 
        __asm__ __volatile__("nop");
    }
}

// Função de comparação para qsort (HWM)
static int compare_int64(const void *a, const void *b) {
    // Retorna negativo se a < b, positivo se a > b, zero se a == b
    int64_t arg1 = *(const int64_t *)a;
    int64_t arg2 = *(const int64_t *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

// Inicializa WiFi
static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {.threshold.authmode = WIFI_AUTH_WPA2_PSK,},
    };
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando ao WiFi %s...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "WiFi Conectado.");
}

// Callback de sincronização SNTP de tempo
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "Tempo sincronizado via SNTP.");
    sntp_sync_cycles = 0; // Zera contador 
}

// Fusão de sensores - Periódica (T = 5ms, D = 5ms, C = 0,8ms)
static void task_fus_imu(void *arg) {
    TickType_t next = xTaskGetTickCount();
    const TickType_t T = pdMS_TO_TICKS(FUS_T_MS);
    for (;;) {
        vTaskDelayUntil(&next, T); // vTaskDelayUntil garante uma periodicidade precisa (determinística), corrigindo jitters
        int64_t start_time = esp_timer_get_time();
        
        // Acesso ao recurso compartilhado (g_state) - Uso do Mutex para simular bloqueio
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(1)) == pdTRUE) {
            // Simulação rápida de filtro (Madgwick/Complementar "fake") + "carga" de 1ms
            g_state.roll  *= 0.98f;  g_state.pitch *= 0.98f;  g_state.yaw *= 0.98f;
            g_state.roll  += 0.1f;   g_state.yaw   += 0.05f;
            xSemaphoreGive(xStateMutex);
        } else {
            // Se falhar, sofreu bloqueio/interferência por recurso (inversão de prioridade)
            g_blocks.fus_imu_blocks++;
            ESP_LOGW(TAG, "FUS_IMU: Bloqueio no Mutex!"); 
        }
        cpu_tight_loop_us(WCET_FUS_IMU_US);

        // Medição do tempo de execução real para WCRT
        int64_t exec_time_us = esp_timer_get_time() - start_time;
        int64_t exec_time_ms = exec_time_us / 1000;
        if (exec_time_ms > fus_imu_wcrt_exec) {
            fus_imu_wcrt_exec = exec_time_ms;
        }

        if (hCTRL) xTaskNotifyGive(hCTRL); // Notifica a tarefa de controle que um novo dado está pronto
        
        // Verifica se a tarefa cumpriu seu deadline
        int64_t exec_time = (esp_timer_get_time() - start_time) / 1000;
        if (exec_time > FUS_IMU_DEADLINE_MS) {
            fus_imu_misses++;
            ESP_LOGE(TAG, "DEADLINE MISS: FUS_IMU (Exec: %lld ms)", exec_time);
        }
    }
}

// Controle de altitude - Ativada pela FUS_IMU (D = 5ms, C = 0,8ms)
static void task_ctrl_att(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // Bloqueia a tarefa até que a FUS_IMU a notifique
        int64_t start_time = esp_timer_get_time();

        // Simulação: Leitura do recurso compartilhado (g_state) - Uso de Mutex
        if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(1)) == pdTRUE) { // Espera máxima de 1ms
            // Simula o cálculo do PID e atuação (leitura de g_state)
            float _roll = g_state.roll; 
            (void)_roll;
            xSemaphoreGive(xStateMutex);
        } else {
            // Se falhar, sofreu bloqueio/interferência por recurso (inversão de prioridade)
            g_blocks.ctrl_att_blocks++;
            ESP_LOGW(TAG, "CTRL_ATT: Bloqueio no Mutex!"); 
        }

        cpu_tight_loop_us(WCET_CTRL_ATT_US); // PID simulado com carga de 0,8ms

        // Medição do tempo de execução real para WCRT
        int64_t exec_time_us = esp_timer_get_time() - start_time;
        int64_t exec_time_ms = exec_time_us / 1000;
        if (exec_time_ms > ctrl_att_wcrt_exec) {
            ctrl_att_wcrt_exec = exec_time_ms;
        }

        // Verifica se a tarefa cumpriu seu deadline
        int64_t exec_time = (esp_timer_get_time() - start_time) / 1000;
        if (exec_time > CTRL_ATT_DEADLINE_MS) {
            ctrl_att_misses++;
            ESP_LOGE(TAG, "DEADLINE MISS: CTRL_ATT (Exec: %lld ms)", exec_time);
        }
    }
}

// Controle de rota/Navegação (Touch B) (D = 20ms, C = 3,5ms) || Telemetria (Touch C) (D = 20ms, C = 0,5ms)
static void task_nav_plan(void *arg) {
    nav_evt_t ev;
    for (;;) {
        if (xQueueReceive(qNav, &ev, portMAX_DELAY) == pdTRUE) { // Bloqueia até receber um evento (Touch B ou C) na Fila
            int64_t task_start_time = esp_timer_get_time();
            int64_t latency = (task_start_time - isr_event_timestamp) / 1000;
            
            if (ev == EV_NAV) { // Controle de rota / Navegação (Touch B)
                ESP_LOGW(TAG, "NAV_PLAN: Touch B (Navegação) ativado. Latência: %lld ms", latency);
                cpu_tight_loop_us(WCET_NAV_PLAN_US); // ~3,5ms
                
            } else if (ev == EV_TEL) { // Telemetria (Touch C)
                ESP_LOGI(TAG, "NAV_PLAN: Touch C (Telemetria) ativado. Latência: %lld ms", latency);

                // Simulação: Leitura do recurso compartilhado (g_state) - Uso de Mutex
                 if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(1)) == pdTRUE) { 
                    printf("TEL: roll=%.2f pitch=%.2f yaw=%.2f\n", g_state.roll, g_state.pitch, g_state.yaw);
                    xSemaphoreGive(xStateMutex);
                } else {
                    g_blocks.nav_plan_blocks++;
                    ESP_LOGW(TAG, "NAV_PLAN: Bloqueio no Mutex!"); 
                    printf("TEL: falha na leitura do estado\n");
                }
                cpu_tight_loop_us(WCET_TELEMETRY_US); // ~0,5ms
            }

            // Verifica se a tarefa cumpriu seu deadline
            int64_t exec_time = (esp_timer_get_time() - task_start_time) / 1000;
            bool deadline_met = ((latency + exec_time) <= NAV_PLAN_DEADLINE_MS);

            // Atualiza as métricas de (m,k)-firm
            nav_total_runs++; // k total
            int is_success = deadline_met ? 1 : 0;
            if (is_success) nav_total_success++; // m total

            // Armazena o resultado no buffer circular
            mk_firm_buffer[mk_firm_index] = is_success; // Adiciona o resultado
            mk_firm_index = (mk_firm_index + 1) % K_WINDOW_SIZE; // Move o índice (desliza a janela)

            // Calcula 'm' atual (total de sucessos na janela de K_WINDOW_SIZE)
            int m_current = 0;
            for (int i = 0; i < K_WINDOW_SIZE; i++) {
                m_current += mk_firm_buffer[i];
            }

            // Verifica a falha (m,k)-firm: a janela de K=10 não tem m=9 sucessos.
            // Verifica se há amostras suficientes (janela cheia) e se o requisito mínimo foi violado
            if (nav_total_runs >= K_WINDOW_SIZE && m_current < NAV_M_MIN) {
                mk_firm_failures++;
                ESP_LOGE(TAG, "M,K-FIRM FAILURE: (%d/%d) misses na janela de %d!", m_current, K_WINDOW_SIZE - m_current, K_WINDOW_SIZE);
            }
            
            // Verifica o deadline
            if (!deadline_met) {
                nav_plan_misses++;
                ESP_LOGE(TAG, "DEADLINE MISS: NAV_PLAN (Total: %lld ms)", latency + exec_time);
            }

            // Coleta estatísticas de latência
            nav_latencies[nav_event_count % HWM_BUFFER_SIZE] = latency;
            nav_event_count++;
            nav_latency_sum += latency;
            if (latency > nav_latency_max) { 
                nav_latency_max = latency; 
            }
            
        }
    }
}

// Fail-safe / Emergência (Touch D) (D = 10ms, C = 0,9ms)
static void task_fail_safe(void *arg) {
    for (;;) {
        if (xSemaphoreTake(semFS, portMAX_DELAY) == pdTRUE) { // Bloqueia até receber o sinal de emergência do ISR
            int64_t task_start_time = esp_timer_get_time();
            int64_t latency = (task_start_time - isr_fs_timestamp) / 1000;
            
            ESP_LOGE(TAG, "FAIL-SAFE: Touch D ativado! Latência: %lld ms", latency);

            // Simula a rotina de pouso seguro.
            cpu_tight_loop_us(WCET_FS_TASK_US);

            // Verifica se a tarefa cumpriu seu deadline.
            int64_t exec_time = (esp_timer_get_time() - task_start_time) / 1000;
            if ((latency + exec_time) > FS_TASK_DEADLINE_MS) {
                fs_task_misses++;
                ESP_LOGE(TAG, "DEADLINE MISS: FS_TASK (Total: %lld ms)", latency + exec_time);
            }
            fs_latencies[fs_event_count % HWM_BUFFER_SIZE] = latency;
            fs_event_count++;
            fs_latency_sum += latency;
            if (latency > fs_latency_max) {
                fs_latency_max = latency;
            }
        }
    }
}

// Perturbação/Sobrecarga (Touch A)
static void task_perturbation(void *arg) {
    for (;;) {
        if (xSemaphoreTake(semPerturbation, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "PERTURBAÇÃO: Touch A ativado. Injetando %d us de carga extra na CPU.", WCET_PERTURBATION_US);
            // Simulação: Acesso ao recurso compartilhado (g_state) com espera longa (simulando inversão de prioridade)
            // Se essa tarefa tiver prioridade baixa, ela pode bloquear tarefas de prioridade alta (FUS_IMU/CTRL_ATT)
            if (xSemaphoreTake(xStateMutex, pdMS_TO_TICKS(50)) == pdTRUE) { 
                 cpu_tight_loop_us(WCET_PERTURBATION_US);
                 xSemaphoreGive(xStateMutex);
            } else {
                 g_blocks.perturbation_blocks++;
                 ESP_LOGE(TAG, "PERTURBAÇÃO: Falha ao obter Mutex (muito ocupado?)"); 
            }
            
            if (hCTRL) xTaskNotifyGive(hCTRL); // Notifica a tarefa de controle que um novo dado está pronto, gerando sobrecarga
        }
    }
}

// Processa o comando recebido
static void process_command(char* rx_buffer) {
    char *p = strstr(rx_buffer, "\n"); if (p) *p = 0; // Remove \n ou \r\n que o cliente Python envia
    p = strstr(rx_buffer, "\r"); if (p) *p = 0;
    
    ESP_LOGI(TAG, "MONITOR_TASK: Comando recebido: %s", rx_buffer);

    if (strcmp(rx_buffer, "FS_TASK") == 0) {
        ESP_LOGW(TAG, "Comando Rede: Ativando FS_TASK!");
        isr_fs_timestamp = esp_timer_get_time(); // Time stamp do evento via Rede
        xSemaphoreGive(semFS); // Ativa o Fail-Safe
        gpio_set_level(LED_GPIO, 1); // Acende o LED
        led_off_time = esp_timer_get_time() + (200 * 1000); // Define tempo para desligar o LED em 200ms
    } else if (strcmp(rx_buffer, "NAV_PLAN") == 0) {
        ESP_LOGW(TAG, "Comando Rede: Ativando NAV_PLAN!");
        isr_event_timestamp = esp_timer_get_time(); // Time stamp do evento via Rede
        nav_evt_t ev = EV_NAV;
        xQueueSend(qNav, &ev, 0); // Ativa a Navegação

    } else if (strcmp(rx_buffer, "TELEMETRY") == 0) {
        ESP_LOGW(TAG, "Comando Rede: Ativando NAV_PLAN (Telemetria)!");
        isr_event_timestamp = esp_timer_get_time(); // Time stamp do evento via Rede
        nav_evt_t ev = EV_TEL; // Envia o evento de Telemetria
        xQueueSend(qNav, &ev, 0); 
    }
}

// Monta o pacote de log em JSON
static int build_log_packet(char* payload_buffer, int max_size) {   
    static int seq = 0;
    // Calcula médias
    int64_t nav_latency_avg = (nav_event_count > 0) ? (nav_latency_sum / nav_event_count) : 0;
    int64_t fs_latency_avg = (fs_event_count > 0) ? (fs_latency_sum / fs_event_count) : 0;
    int64_t rtt_latency_avg = (rtt_count > 0) ? (rtt_sum / rtt_count) : 0;
    
    // Pega a hora atual (protegida)
    time_t current_time_epoch = 0;
    if (xSemaphoreTake(xTimeMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        current_time_epoch = g_now;
        xSemaphoreGive(xTimeMutex);
    }
    
    // Monta o pacote de log
    return snprintf(payload_buffer, max_size,
                "{\"seq\":%d, \"policy\":\"%s\", \"time_epoch\":%lld, \"rtt_ms\":%lld, "
                "\"hard_misses\":{\"fus\":%d, \"ctrl\":%d, \"fs\":%d}, "
                "\"soft_misses\":{\"nav\":%d}, \"mk_firm_fails\":%d, "
                "\"blocks\":{\"fus\":%d, \"ctrl\":%d, \"nav\":%d, \"pert\":%d, \"time\":%d}, " 
                "\"fs_latency\":{\"avg\":%lld, \"max\":%lld, \"count\":%llu}, "
                "\"nav_latency\":{\"avg\":%lld, \"max\":%lld, \"count\":%llu}}\n",
                seq++, POLICY_NAME, (long long)current_time_epoch, (long long)rtt_latency_avg,
                fus_imu_misses, ctrl_att_misses, fs_task_misses,
                nav_plan_misses, mk_firm_failures,
                g_blocks.fus_imu_blocks, g_blocks.ctrl_att_blocks,
                g_blocks.nav_plan_blocks, g_blocks.perturbation_blocks, g_blocks.time_blocks,
                fs_latency_avg, fs_latency_max, fs_event_count,
                nav_latency_avg, nav_latency_max, nav_event_count);
}

// Captura a hora de um servidor SNTP e monitora a sincronização - Periódica (T = 1s, D = 1s)
static void time_task(void *arg) {
    // Aguarda a primeira sincronização antes de entrar no loop
    ESP_LOGI(TAG, "TIME_TASK: Aguardando primeira sincronização SNTP...");
    esp_sntp_set_sync_interval(pdMS_TO_TICKS(15000)); // 15s
    for (int i=0; i<30 && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
         ESP_LOGE(TAG, "TIME_TASK: Falha ao sincronizar SNTP. Verifique WiFi/NTP.");
    } else {
         last_sync_status = SNTP_SYNC_STATUS_COMPLETED;
    }

    TickType_t next = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(TIME_DEADLINE_MS)); // Período de 1s

        time_t now_local = 0;
        struct tm timeinfo_local = {0};
        time(&now_local);
        localtime_r(&now_local, &timeinfo_local);

        // Trava o mutex para atualizar as variáveis globais de tempo
        if (xSemaphoreTake(xTimeMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            g_now = now_local;
            g_timeinfo = timeinfo_local;
            xSemaphoreGive(xTimeMutex);
        } else {
            g_blocks.time_blocks++;
            ESP_LOGW(TAG, "TIME_TASK: Bloqueio no xTimeMutex! (Total: %d)", g_blocks.time_blocks);
        }
        
        sntp_sync_status_t current_sync_status = esp_sntp_get_sync_status();

        // Monitora ciclos desde a última atualização e avisa sobre sincronização
        if (current_sync_status == SNTP_SYNC_STATUS_RESET) {
            sntp_sync_cycles++;
            if (sntp_sync_cycles % 5 == 0) { // Avisa a cada 5 ciclos (5s)
                ESP_LOGW(TAG, "TIME_TASK: SNTP não sincronizado há %d ciclos (5s).", sntp_sync_cycles);
            }
        } else if (current_sync_status == SNTP_SYNC_STATUS_COMPLETED && last_sync_status != SNTP_SYNC_STATUS_COMPLETED) {
            // Loga a sincronização recorrente
            ESP_LOGI(TAG, "TIME_TASK: Sincronização SNTP RECORRENTE OK.");
            sntp_sync_cycles = 0;
        }

        last_sync_status = current_sync_status;
    }
}

#ifndef MONITOR_MODE_TCP
// MONITOR_TASK como Cliente UDP - envia logs para o PC a cada 500ms e recebe comandos a qualquer momento.
static void monitor_task_udp(void *arg) {
    char rx_buffer[256]; // Buffer para receber comandos
    char payload[512];   // Buffer para o JSON
    int64_t last_log_send_time = 0; // Timestamp do último envio de log
    const int loop_delay_ms   = 50;  // Delay do loop principal
    const int log_interval_ms = 500; // Intervalo de envio de logs

    // Configura o endereço do Servidor (PC)
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PC_SERVER_PORT);
    inet_pton(AF_INET, PC_SERVER_IP, &dest_addr.sin_addr.s_addr);

    // Cria o socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "MONITOR (UDP): Falha ao criar socket");
        vTaskDelete(NULL);
        return;
    }

    // Define timeout de recebimento para não bloquear o loop
    struct timeval tv = {0, 1000}; // 1ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "MONITOR_TASK (UDP): Enviando logs para %s:%d", PC_SERVER_IP, PC_SERVER_PORT);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(loop_delay_ms)); // Loop rápido

        int64_t now_ms = esp_timer_get_time() / 1000; // Tempo atual em ms

        // Enviar Logs (Periódico)
        if ((now_ms - last_log_send_time) >= log_interval_ms) {
            int len = build_log_packet(payload, sizeof(payload));
            int64_t send_time = esp_timer_get_time(); // Timestamp de envio para RTT

            sendto(sock, payload, len, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            last_log_send_time = now_ms;

            // Tentativa de Receber Resposta Imediata para RTT (Requer que o Servidor PC responda)
            struct sockaddr_in source_addr; 
            socklen_t socklen = sizeof(source_addr);
            // Configura timeout temporário para a resposta RTT
            struct timeval rtt_tv = {0, 10 * 1000}; // 10ms (Tempo de ida e volta esperado)
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtt_tv, sizeof(rtt_tv));

            int len_resp = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

            // Volta ao timeout rápido de 1ms
            struct timeval fast_tv = {0, 1000}; 
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &fast_tv, sizeof(fast_tv));

            if (len_resp > 0) {
                int64_t rtt = (esp_timer_get_time() - send_time) / 1000; // RTT em ms
                rtt_count++;
                rtt_sum += rtt;
                if (rtt > rtt_max) rtt_max = rtt;
                ESP_LOGI(TAG, "MONITOR (UDP): RTT de %lld ms", rtt);
            }
        }

        // Receber Comandos (Não-bloqueante) 
        struct sockaddr_in source_addr; 
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = 0;
            process_command(rx_buffer);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}
#else
// MONITOR_TASK como Cliente TCP - Envia logs a cada 500ms e recebe comandos a qualquer momento.
static void monitor_task_tcp(void *arg) {
    char rx_buffer[256]; // Buffer para receber comandos
    char payload[512];   // Buffer para o JSON
    const int loop_delay_ms   = 50;  // Delay do loop principal
    const int log_interval_ms = 500; // Intervalo de envio de logs

    // Configura o endereço do Servidor (PC)
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PC_SERVER_PORT);
    inet_pton(AF_INET, PC_SERVER_IP, &dest_addr.sin_addr.s_addr);

    ESP_LOGI(TAG, "MONITOR_TASK (TCP): Tentando conectar em %s:%d", PC_SERVER_IP, PC_SERVER_PORT);

    while (1) { // Loop de reconexão
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "MONITOR (TCP): Falha ao criar socket");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Tenta conectar ao PC (Servidor)
        if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) != 0) {
            ESP_LOGE(TAG, "MONITOR (TCP): Falha ao conectar. Tentando novamente em 5s...");
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "MONITOR (TCP): Conectado ao PC (Servidor)");
        
        // Define timeout de recebimento para não bloquear o loop
        struct timeval tv = {0, 1000}; // 1ms
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        int64_t last_log_send_time = 0;

        while (1) { // Loop de comunicação
            vTaskDelay(pdMS_TO_TICKS(loop_delay_ms)); // Loop rápido

            if (led_off_time > 0 && esp_timer_get_time() >= led_off_time) {
                gpio_set_level(LED_GPIO, 0);
                led_off_time = 0;
            }

            int64_t now = esp_timer_get_time() / 1000; // Tempo atual em ms

            // Enviar Logs (periódico)
            if ((now - last_log_send_time) >= log_interval_ms) {
                // Para RTT no TCP: o timestamp de envio é anexado ao JSON
                int len = build_log_packet(payload, sizeof(payload));
    
                int64_t send_time = esp_timer_get_time(); // Timestamp de envio para RTT
                int bytes_sent = send(sock, payload, len, 0);

                if (bytes_sent > 0) {
                    struct timeval rtt_tv = {0, 50 * 1000}; // Espera o ECO por 50ms
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtt_tv, sizeof(rtt_tv));
                    
                    // Espera pela resposta 'ECO'
                    int len_resp = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);

                    struct timeval fast_tv = {0, 1000}; // 1ms
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &fast_tv, sizeof(fast_tv));

                    if (len_resp > 0) { // Resposta ECO recebida (ou outro pacote)
                        int64_t rtt = (esp_timer_get_time() - send_time) / 1000; // RTT em ms
                        rtt_count++;
                        rtt_sum += rtt;
                        if (rtt > rtt_max) rtt_max = rtt;
                        
                        ESP_LOGI(TAG, "MONITOR (TCP): RTT medido: %lld ms", rtt);
                    } else if (len_resp == 0) {
                        // Conexão fechada durante a espera pelo ECO
                        ESP_LOGW(TAG, "MONITOR (TCP): Conexão fechada pelo PC durante medição RTT.");
                        break; 
                    } // Se len_resp < 0, foi timeout ou erro, RTT falhou (ignorar o sample)

                    last_log_send_time = now;
                } else if (bytes_sent < 0) {
                    ESP_LOGE(TAG, "MONITOR (TCP): Falha no envio. Conexão perdida.");
                    break; // Sai do loop de comunicação para reconectar
                }
            }

            // Receber Comandos (não-bloqueante) 
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);

            if (len > 0) { // Comando recebido
                rx_buffer[len] = 0;
                // Ignora o ECO do servidor para não ser processado como comando
                if (strncmp(rx_buffer, "ECO", 3) != 0) { 
                    process_command(rx_buffer);
                } else {
                    // Caso o ECO tenha sido lido pelo loop rápido
                    ESP_LOGI(TAG, "MONITOR (TCP): ECO descartado no loop rápido.");
                }
            } else if (len == 0) { // Conexão fechada pelo Servidor (PC)
                ESP_LOGW(TAG, "MONITOR (TCP): Conexão fechada pelo PC.");
                break; // Sai do loop de comunicação para reconectar
            }
        }
        // Se saiu do loop de comunicação, fecha o socket e tenta reconectar
        shutdown(sock, 0);
        close(sock);
        ESP_LOGI(TAG, "MONITOR (TCP): Socket fechado.");
    }
    vTaskDelete(NULL);
}
#endif

// ISR (Rotina de Serviço de Interrupção) - Detecção capacitiva de toque
static void touch_isr_handler(void *arg) {
    uint32_t pad_intr = touch_pad_get_status();
    touch_pad_clear_status();
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Debounce
    int64_t current_time = esp_timer_get_time();
    if ((current_time - last_isr_time) < (DEBOUNCE_DELAY_MS * 1000)) {
        touch_pad_clear_status();
        return;
    }
    last_isr_time = current_time;

    // Detecção capacitiva de toque e envio de eventos para a tarefa correspondente
    // if (pad_intr & (1 << TOUCH_B)) {
    //     isr_event_timestamp = esp_timer_get_time();
    //     nav_evt_t ev = EV_NAV;
    //     xQueueSendFromISR(qNav, &ev, &xHigherPriorityTaskWoken);
    // }
    // if (pad_intr & (1 << TOUCH_C)) {
    //     isr_event_timestamp = esp_timer_get_time();
    //     nav_evt_t ev = EV_TEL;
    //     xQueueSendFromISR(qNav, &ev, &xHigherPriorityTaskWoken);
    // }
    // if (pad_intr & (1 << TOUCH_D)) {
    //     isr_fs_timestamp = esp_timer_get_time();
    //     xSemaphoreGiveFromISR(semFS, &xHigherPriorityTaskWoken);
    // }
    if (pad_intr & (1 << TOUCH_A)) {
        xSemaphoreGiveFromISR(semPerturbation, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// Funcao auxiliar para calcular HWM
// Retorna o valor do percentil desejado (ex: 0.99 para HWM 99%) // -1 se count=0 ou falhar
static int64_t calculate_hwm_value(int64_t* buffer, uint64_t count, float percentile) {
    // Usando apenas 10 como mínimo para cálculo de HWM, se necessário
    if (count == 0 || count < 10) return -1;
    
    // Limita o tamanho ao buffer circular ou ao total de eventos
    uint64_t size = (count < HWM_BUFFER_SIZE) ? count : HWM_BUFFER_SIZE;
    
    // Aloca memória dinamicamente
    int64_t *hwm_buffer = (int64_t *)malloc(size * sizeof(int64_t));
    if (!hwm_buffer) return -1; 
    
    // Copia os dados do buffer circular para o buffer temporário
    uint64_t start_index = (count > HWM_BUFFER_SIZE) ? (count % HWM_BUFFER_SIZE) : 0;
    for (uint64_t i = 0; i < size; i++) {
        hwm_buffer[i] = buffer[(start_index + i) % HWM_BUFFER_SIZE];
    }

    // Ordena o buffer
    qsort(hwm_buffer, size, sizeof(int64_t), compare_int64);

    // Calcula o índice do percentil (ex: 99% de 1000 = 990; índice 989)
    // O índice deve ser no mínimo 0.
    uint64_t index = (uint64_t)(size * percentile);
    if (index > 0) index--;
    
    int64_t hwm_value = hwm_buffer[index];
    
    // Libera a memória
    free(hwm_buffer);
    return hwm_value;
}

// Relatório automático após 60 segundos de teste
void task_report(void *pvParameters) {
    const int test_duration_ms = 60000; // Duração do teste (60 segundos)
    ESP_LOGI(TAG, "TASK_REPORT: Teste de 60 segundos iniciado. Pressione os botões e envie comandos...");
    
    vTaskDelay(pdMS_TO_TICKS(test_duration_ms)); // Dorme pelo tempo do teste

    printf("\n\n==============================================\n");
    printf("RELATÓRIO (60 segundos)\n");
    printf("==============================================\n");

    // Configuração
    printf("Configuração:\n");
    printf("  - Política de Escalonamento: %s\n", POLICY_NAME);
    #ifdef MONITOR_MODE_TCP
        printf("  - Protocolo de Rede: TCP (Cliente)\n");
    #else
        printf("  - Protocolo de Rede: UDP (Cliente)\n");
    #endif

    // Deadline Misses (Hard e Soft)
    printf("----------------------------------------------\n");
    printf("Deadline Misses:\n");
    printf("  - FUS_IMU  (Hard RT): %d\n", fus_imu_misses);
    printf("  - CTRL_ATT (Hard RT): %d\n", ctrl_att_misses);
    printf("  - FS_TASK  (Hard RT): %d\n", fs_task_misses);
    printf("  - NAV_PLAN (Soft RT): %d\n", nav_plan_misses);
    printf("  - Total Hard Misses: %d\n", fus_imu_misses + ctrl_att_misses + fs_task_misses);
    printf("  - Total Soft Misses: %d\n", nav_plan_misses);

    // Total geral de bloqueios
    int total_blocks = g_blocks.fus_imu_blocks + g_blocks.ctrl_att_blocks + 
                       g_blocks.nav_plan_blocks + g_blocks.perturbation_blocks + 
                       g_blocks.time_blocks;

    printf("----------------------------------------------\n");
    printf("Análise de Bloqueio e Inversão de Prioridade:\n");
    printf("  - Total Geral de Bloqueios: %d\n", total_blocks);
    printf("\n");
    printf("  Bloqueios por Tarefa (xStateMutex):\n");
    printf("    • FUS_IMU:       %d bloqueios\n", g_blocks.fus_imu_blocks);
    printf("    • CTRL_ATT:      %d bloqueios\n", g_blocks.ctrl_att_blocks);
    printf("    • NAV_PLAN:      %d bloqueios\n", g_blocks.nav_plan_blocks);
    printf("    • PERTURBATION:  %d bloqueios\n", g_blocks.perturbation_blocks);
    printf("\n");
    printf("  Bloqueios em Outros Mutexes:\n");
    printf("    • TIME_TASK (xTimeMutex): %d bloqueios\n", g_blocks.time_blocks);
    printf("\n");

    // WCRT/Latência de Eventos e Execução
    printf("----------------------------------------------\n");
    printf("WCRT de Execução (Tempo Real Mais Longo):\n");
    printf("  - FUS_IMU (Execução): %lld ms (D=5ms)\n", fus_imu_wcrt_exec);
    printf("  - CTRL_ATT (Execução): %lld ms (D=5ms)\n", ctrl_att_wcrt_exec);
    printf("Latência de Eventos (ISR -> Tarefa):\n");

    // Latência da Navegação/Telemetria (Touch B/C)
    int64_t nav_latency_avg = (nav_event_count > 0) ? (nav_latency_sum / nav_event_count) : 0;
    printf("  - Navegação/Telemetria (Touch B/C):\n");
    printf("      Latência Média:      %lld ms\n", nav_latency_avg);
    printf("      WCRT (Lat. Máxima):  %lld ms\n", nav_latency_max);

    // Latência do Fail-Safe (Touch D)
    int64_t fs_latency_avg = (fs_event_count > 0) ? (fs_latency_sum / fs_event_count) : 0;
    printf("  - Fail-Safe (Touch D):\n");
    printf("      Latência Média:      %lld ms\n", fs_latency_avg);
    printf("      WCRT (Lat. Máxima):  %lld ms\n", fs_latency_max);

    // Cálculo de HWM (High Water Mark)
    printf("----------------------------------------------\n");
    printf("HWM (High Water Mark) - Latência de Eventos:\n");
    
    // Cálculos de HWM 99%
    int64_t fs_hwm99 = calculate_hwm_value(fs_latencies, fs_event_count, 0.99f);
    int64_t nav_hwm99 = calculate_hwm_value(nav_latencies, nav_event_count, 0.99f);

    if (fs_hwm99 < 0) {
        printf("  - Fail-Safe (HWM 99%%): N/A (amostras insuficientes)\n");
    } else {
        printf("  - Fail-Safe (HWM 99%%): %lld ms\n", fs_hwm99);
    }
    
    if (nav_hwm99 < 0) {
        printf("  - Navegação (HWM 99%%): N/A (amostras insuficientes)\n");
    } else {
        printf("  - Navegação (HWM 99%%): %lld ms\n", nav_hwm99);
    }

    // Outros parâmetros
    printf("----------------------------------------------\n");
    printf("Outros Parâmetros:\n");

    // Resultado do (m,k)-firm: Janela Deslizante
    printf("  - (m,k)-firm (Janela Deslizante):\n");
    printf("      Janela k = %d; Requisito m = %d ((%d,%d)-firm)\n", K_WINDOW_SIZE, NAV_M_MIN, NAV_M_MIN, K_WINDOW_SIZE);
    printf("      Total de Falhas na Janela:  %d\n", mk_firm_failures);
    
    // Mantendo a taxa global para referência estatística no relatório (item c)
    if (nav_total_runs > 0) {
        float success_rate = (float)nav_total_success / nav_total_runs * 100.0f;
        printf("Taxa de Sucesso Global: %.2f%% (%llu/%llu)\n", 
            success_rate, (unsigned long long)nav_total_success, (unsigned long long)nav_total_runs);
    } else {
        printf("Taxa de Sucesso Global: N/A (Nenhum evento NAV_PLAN registrado).\n");
    }

    // Latência Protocolo (RTT)
    int64_t rtt_latency_avg = (rtt_count > 0) ? (rtt_sum / rtt_count) : 0;
    printf("  - Latência Protocolo (RTT) Média: %lld ms\n", rtt_latency_avg); 
    printf("  - Latência Protocolo (RTT) Máxima: %lld ms\n", rtt_max);
    
    // Análise de Bloqueio/Interferência
    printf("  - Contagem de Bloqueio (Inversão de Prioridade): %d\n", total_blocks);
    printf("==============================================\n");

    // Deleta a si mesma, as outras tarefas continuam.
    vTaskDelete(NULL);
}

// Função principal: executada apenas uma vez para configurar e iniciar o sistema.
void app_main(void) {
    ESP_LOGI(TAG, "Inicializando Autopiloto Didático STR...");
    ESP_LOGI(TAG, "Política de Escalonamento: %s", POLICY_NAME);

    // Inicializa a Rede e SNTP
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init(); // Conecta ao WiFi
    ESP_LOGI(TAG, "Inicializando SNTP...");
    setenv("TZ", "BRT-3", 1); // Fuso Horário Brasil
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_setservername(0, "a.st1.ntp.br");
    esp_sntp_setservername(1, "b.st1.ntp.br");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_sync_interval(pdMS_TO_TICKS(15000)); // Sincroniza a cada 15 segundos
    esp_sntp_init();

    // Configura o LED
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    // Inicializa os mecanismos de comunicação (IPC)
    qNav  = xQueueCreate(8, sizeof(nav_evt_t));
    semFS = xSemaphoreCreateBinary();
    semPerturbation = xSemaphoreCreateBinary();
    xTimeMutex = xSemaphoreCreateMutex();
    xStateMutex = xSemaphoreCreateMutex();

    // Configuração dos touch pads
    touch_pad_init();
    touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER);
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    touch_pad_config(TOUCH_B, 0);
    touch_pad_config(TOUCH_C, 0);
    touch_pad_config(TOUCH_D, 0);
    touch_pad_config(TOUCH_A, 0);
    touch_pad_filter_start(10);
    uint16_t touch_value;
    touch_pad_read_filtered(TOUCH_B, &touch_value);
    touch_pad_set_thresh(TOUCH_B, touch_value * 0.4);
    touch_pad_read_filtered(TOUCH_C, &touch_value);
    touch_pad_set_thresh(TOUCH_C, touch_value * 0.4);
    touch_pad_read_filtered(TOUCH_D, &touch_value);
    touch_pad_set_thresh(TOUCH_D, touch_value * 0.4);
    touch_pad_read_filtered(TOUCH_A, &touch_value);
    touch_pad_set_thresh(TOUCH_A, touch_value * 0.4);
    touch_pad_isr_register(touch_isr_handler, NULL);
    touch_pad_intr_enable();

    // Cria as tarefas e entrega o controle ao escalonador do FreeRTOS.
    xTaskCreatePinnedToCore(task_fail_safe,      "FS_TASK",      STK, NULL, PRIO_FS_TASK,  &hFS,   0);
    xTaskCreatePinnedToCore(task_fus_imu,        "FUS_IMU",      STK, NULL, PRIO_FUS_IMU,  &hFUS,  0);
    xTaskCreatePinnedToCore(task_ctrl_att,       "CTRL_ATT",     STK, NULL, PRIO_CTRL_ATT, &hCTRL, 0);
    xTaskCreatePinnedToCore(task_nav_plan,       "NAV_PLAN",     STK, NULL, PRIO_NAV_PLAN, &hNAV,  0);
    xTaskCreatePinnedToCore(task_perturbation,   "PERTURB_TASK", STK, NULL, PRIO_PERTURBATION, NULL, 0);
    xTaskCreatePinnedToCore(time_task,           "TIME_TASK",   4096, NULL, PRIO_TIME, NULL, 0);
    xTaskCreatePinnedToCore(task_report,         "REPORT_TASK", 4096, NULL, 2, NULL, 0);

    // Cria a MONITOR_TASK com base no #define
    #ifdef MONITOR_MODE_TCP
        ESP_LOGI(TAG, "Modo de Rede: Cliente TCP ATIVO.");
        xTaskCreatePinnedToCore(monitor_task_tcp, "MONITOR_TASK", 4096, NULL, PRIO_MONITOR, NULL, 0);
    #else
        ESP_LOGI(TAG, "Modo de Rede: Cliente UDP ATIVO.");
        xTaskCreatePinnedToCore(monitor_task_udp, "MONITOR_TASK", 4096, NULL, PRIO_MONITOR, NULL, 0);
    #endif

    ESP_LOGI(TAG, "Sistema inicializado. Tarefas criadas e em execução no Core 0.");
    vTaskDelete(NULL);
}