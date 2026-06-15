#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/usart.h>

// --- Definiciones del Sistema de Buzones (MVP) ---
#define CANT_BUZONES 5
#define LARGO_PIN 4

uint8_t codigos_maestros[CANT_BUZONES][LARGO_PIN] = {
    {1, 1, 1, 1}, // Buzón 1 
    {2, 2, 2, 2}, // Buzón 2
    {3, 3, 3, 3}, // Buzón 3
    {4, 4, 4, 4}, // Buzón 4
    {5, 5, 5, 5}  // Buzón 5
};
uint8_t buzones_ocupados[CANT_BUZONES] = {1, 1, 1, 1, 1}; // Arrancan ocupados
uint8_t pin_usuario[LARGO_PIN] = {0, 0, 0, 0};
uint8_t digitos_ingresados = 0;
uint8_t prev_boton[5] = {1, 1, 1, 1, 1};

// --- Constantes del Protocolo TP5 ---
#define PROTOCOL_MAX_PAYLOAD_LENGTH 48
#define PROTOCOL_MAX_BODY_SIZE      52  
#define PROTOCOL_MAX_FRAME_LENGTH   64
#define PROTOCOL_TYPE_CMD           "CMD"

// --- Contadores del Protocolo TP5 ---
uint32_t rx_count = 0;
uint32_t ae_count = 0;
uint32_t irq_count = 0;
uint32_t pb_count = 0;
uint32_t pm_count = 0;
uint32_t pe_count = 0;
uint32_t qd_count = 0; 

// --- Estructuras del Parser (FSM) ---
typedef enum {
    PARSER_STATE_WAIT_START,
    PARSER_STATE_READ_LEN_HI,
    PARSER_STATE_READ_LEN_LO,
    PARSER_STATE_EXPECT_LEN_SEPARATOR,
    PARSER_STATE_READ_BODY,
    PARSER_STATE_EXPECT_CHECK_SEPARATOR,
    PARSER_STATE_READ_CHECK_HI,
    PARSER_STATE_READ_CHECK_LO,
    PARSER_STATE_EXPECT_END
} parser_state_t;

typedef enum {
    PARSER_RESULT_IN_PROGRESS,
    PARSER_RESULT_MESSAGE_READY,
    PARSER_RESULT_ERROR
} parser_result_t;

typedef struct {
    parser_state_t state;
    uint8_t body_len;
    uint8_t body_pos;
    char body_buf[PROTOCOL_MAX_BODY_SIZE + 1];
    uint8_t check_expected;
    uint8_t check_hi_nibble;
} parser_t;

typedef struct {
    char type[4];
    char payload[PROTOCOL_MAX_PAYLOAD_LENGTH + 1];
} protocol_message_t;

parser_t uart_parser;
char response_buf[PROTOCOL_MAX_FRAME_LENGTH + 1];

// --- Prototipos de Hardware y Protocolo ---
void clock_setup(void);
void gpio_setup(void);
void usart_setup(void);
void tim3_setup(void);
void adc_setup(void);

static int hex_char_to_nibble(char c);
uint8_t protocol_checksum(const char *input, size_t len);
int protocol_encode(const char *type, const char *payload, char *buf, size_t buf_size);
int protocol_validate(const char *frame, size_t frame_len);
void parser_init(parser_t *p);
parser_result_t parser_consume_byte(parser_t *p, uint8_t byte, protocol_message_t *msg);
void app_handle_message(const protocol_message_t *msg);
void uart_tx_send(const char *str);
void procesar_uart_fsm(void);

// --- Prototipos del MVP ---
void emitir_feedback(uint32_t duracion);
void procesar_pin(void);
void abrir_buzon(int num_buzon);

// --- MAIN ---
int main(void) {
    clock_setup();
    gpio_setup();
    usart_setup();
    adc_setup();
    tim3_setup(); 

    parser_init(&uart_parser);

    // Saludo inicial
    char saludo_buf[PROTOCOL_MAX_FRAME_LENGTH + 1];
    protocol_encode("ACK", "hola, Bienvenido a mi bluepill", saludo_buf, sizeof(saludo_buf));
    uart_tx_send(saludo_buf);
    uart_tx_send("\r\n");

    while (1) {
        // 1. Escuchar UART
        procesar_uart_fsm(); 

        // 2. Lógica de escaneo de botones físicos del Buzón (PA0 a PA4)
        for (int i = 0; i < 5; i++) {
            uint8_t lectura = gpio_get(GPIOA, (1U << i)) ? 1 : 0;

            if (prev_boton[i] == 1 && lectura == 0) { // Flanco de bajada
                for (volatile int j = 0; j < 30000; j++); // Debounce software

                if (!gpio_get(GPIOA, (1U << i))) {
                    if (digitos_ingresados < LARGO_PIN) {
                        pin_usuario[digitos_ingresados] = i + 1;
                        digitos_ingresados++;
                        emitir_feedback(50000); // Aviso sonoro
                    }
                }
            }
            prev_boton[i] = lectura;
        }

        // 3. Evaluar apertura de buzones físicos
        if (digitos_ingresados == LARGO_PIN) {
            procesar_pin();
        }
    }
    return 0;
}

// --- LOGICA DE PROTOCOLO Y PARSER (Etapa 1 y 2) ---

static int hex_char_to_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t protocol_checksum(const char *input, size_t len) {
    uint8_t cs = 0U;
    for (size_t i = 0U; i < len; i++) {
        cs ^= (uint8_t)input[i];
    }
    return cs;
}

int protocol_encode(const char *type, const char *payload, char *buf, size_t buf_size) {
    size_t type_len = strlen(type);
    size_t payload_len = strlen(payload);
    if (type_len != 3) return -1;
    if (payload_len > PROTOCOL_MAX_PAYLOAD_LENGTH) return -1;
    for (size_t i = 0; i < payload_len; i++) {
        if (payload[i] < 0x20 || payload[i] > 0x7E || payload[i] == '@') return -1;
    }
    char body[PROTOCOL_MAX_BODY_SIZE + 1];
    snprintf(body, sizeof(body), "%s:%s", type, payload);
    size_t body_len = strlen(body);

    char cs_input[PROTOCOL_MAX_FRAME_LENGTH + 1];
    int cs_bytes = snprintf(cs_input, sizeof(cs_input), "%02X:%s", (unsigned int)body_len, body);
    if (cs_bytes < 0 || (size_t)cs_bytes >= sizeof(cs_input)) return -1;

    uint8_t cs = protocol_checksum(cs_input, (size_t)cs_bytes);
    int written = snprintf(buf, buf_size, "@%s:%02X\n", cs_input, cs);
    if (written < 0 || (size_t)written >= buf_size) return -1;
    return written;
}

int protocol_validate(const char *frame, size_t frame_len) {
    const char *last_colon = NULL;
    for (size_t i = 0; i < frame_len; i++) {
        if (frame[i] == ':') last_colon = &frame[i];
    }
    if (last_colon == NULL) return -1;
    if (last_colon + 3 != frame + frame_len) return -1;

    const char *cs_received_str = last_colon + 1;
    int hi = hex_char_to_nibble(cs_received_str[0]);
    int lo = hex_char_to_nibble(cs_received_str[1]);
    if (hi < 0 || lo < 0) return -1;
    uint8_t cs_received = (uint8_t)((hi << 4) | lo);

    size_t cs_input_len = (size_t)(last_colon - frame);
    uint8_t cs_calculated = protocol_checksum(frame, cs_input_len);
    return (cs_calculated == cs_received) ? 0 : -1;
}

void parser_init(parser_t *p) {
    p->state = PARSER_STATE_WAIT_START;
    p->body_len = 0;
    p->body_pos = 0;
    p->check_expected = 0;
    p->check_hi_nibble = 0;
    memset(p->body_buf, 0, sizeof(p->body_buf));
}

parser_result_t parser_consume_byte(parser_t *p, uint8_t byte, protocol_message_t *msg) {
    pb_count++;
    if (byte == '\r') return PARSER_RESULT_IN_PROGRESS; 
    switch (p->state) {
        case PARSER_STATE_WAIT_START:
            if (byte == '@') { parser_init(p); p->state = PARSER_STATE_READ_LEN_HI; }
            break;
        case PARSER_STATE_READ_LEN_HI: {
            int val = hex_char_to_nibble(byte);
            if (val >= 0) { p->body_len = (val << 4); p->state = PARSER_STATE_READ_LEN_LO; } else goto error;
            break;
        }
        case PARSER_STATE_READ_LEN_LO: {
            int val = hex_char_to_nibble(byte);
            if (val >= 0) { p->body_len |= val; p->state = PARSER_STATE_EXPECT_LEN_SEPARATOR; } else goto error;
            break;
        }
        case PARSER_STATE_EXPECT_LEN_SEPARATOR:
            if (byte == ':') { p->state = PARSER_STATE_READ_BODY; } else goto error;
            break;
        case PARSER_STATE_READ_BODY:
            if (p->body_pos < PROTOCOL_MAX_BODY_SIZE && p->body_pos < p->body_len) {
                p->body_buf[p->body_pos++] = byte;
                if (p->body_pos == p->body_len) { p->body_buf[p->body_pos] = '\0'; p->state = PARSER_STATE_EXPECT_CHECK_SEPARATOR; }
            } else goto error;
            break;
        case PARSER_STATE_EXPECT_CHECK_SEPARATOR:
            if (byte == ':') p->state = PARSER_STATE_READ_CHECK_HI; else goto error;
            break;
        case PARSER_STATE_READ_CHECK_HI: {
            int val = hex_char_to_nibble(byte);
            if (val >= 0) { p->check_hi_nibble = (val << 4); p->state = PARSER_STATE_READ_CHECK_LO; } else goto error;
            break;
        }
        case PARSER_STATE_READ_CHECK_LO: {
            int val = hex_char_to_nibble(byte);
            if (val >= 0) { p->check_expected = p->check_hi_nibble | val; p->state = PARSER_STATE_EXPECT_END; } else goto error;
            break;
        }
        case PARSER_STATE_EXPECT_END:
            if (byte == '\n') {
                char frame_to_validate[PROTOCOL_MAX_FRAME_LENGTH];
                snprintf(frame_to_validate, sizeof(frame_to_validate), "%02X:%s:%02X", p->body_len, p->body_buf, p->check_expected);
                if (protocol_validate(frame_to_validate, strlen(frame_to_validate)) == 0) {
                    if (p->body_len >= 5 && p->body_buf[3] == ':') {
                        strncpy(msg->type, p->body_buf, 3);
                        msg->type[3] = '\0';
                        strncpy(msg->payload, p->body_buf + 4, PROTOCOL_MAX_PAYLOAD_LENGTH);
                        msg->payload[PROTOCOL_MAX_PAYLOAD_LENGTH] = '\0';
                        pm_count++;
                        parser_init(p);
                        return PARSER_RESULT_MESSAGE_READY;
                    }
                }
            }
            goto error;
    }
    return PARSER_RESULT_IN_PROGRESS;
error:
    pe_count++;
    parser_init(p);
    if (byte == '@') p->state = PARSER_STATE_READ_LEN_HI;
    return PARSER_RESULT_ERROR;
}

void uart_tx_send(const char *str) {
    while (*str) usart_send_blocking(USART1, *str++);
}

void procesar_uart_fsm(void) {
    if ((USART_SR(USART1) & USART_SR_RXNE) == 0) return;
    uint8_t byte = usart_recv(USART1);
    irq_count++; 

    // Eco interactivo para PuTTY
    if (byte == '\r') {
        usart_send_blocking(USART1, '\r'); usart_send_blocking(USART1, '\n');
    } else {
        usart_send_blocking(USART1, byte); 
    }

    protocol_message_t msg;
    parser_result_t result = parser_consume_byte(&uart_parser, byte, &msg);
    
    if (result == PARSER_RESULT_MESSAGE_READY) {
        uart_tx_send("\r\n[FSM] Comando recibido. Despachando:\r\n");
        app_handle_message(&msg);
    }else if(result == PARSER_RESULT_ERROR){
        uart_tx_send("\r\n[FSM] Comando erroneo.\r\n");
        protocol_encode("ERR", "format_error", response_buf, sizeof(response_buf));
        uart_tx_send(response_buf);
        uart_tx_send("\r\n");
    }
}

// --- MANEJO DE COMANDOS (FUSIÓN UART + MVP) ---
void app_handle_message(const protocol_message_t *msg) {
    // 1. Comando: PING (Del TP5)
    if (strcmp(msg->type, "CMD") == 0 && strcmp(msg->payload, "ping") == 0) {
        rx_count++;
        protocol_encode("ACK", "pong=1", response_buf, sizeof(response_buf));
        uart_tx_send(response_buf);
        return;
    }
    // 1.5. Comando: STATUS (Del TP5)
    if (strcmp(msg->type, "CMD") == 0 && strcmp(msg->payload, "status?") == 0) {
        rx_count++;
        char status_payload[128];
        // Mapear las variables a: rx, ae, irq, pb, pm, pe, qd        
        snprintf(status_payload, sizeof(status_payload), 
                "rx=%lu,ae=%lu,irq=%lu,pb=%lu,pm=%lu,pe=%lu,qd=%lu",
                (unsigned long)rx_count, 
                (unsigned long)ae_count, 
                (unsigned long)irq_count,
                (unsigned long)pb_count, 
                (unsigned long)pm_count, 
                (unsigned long)pe_count, 
                (unsigned long)qd_count);

        protocol_encode("STS", status_payload, response_buf, sizeof(response_buf));
        uart_tx_send(response_buf);
        return;
    }

    // 2. Comando: LED (Del TP5)
    if (strcmp(msg->type, "CMD") == 0) {
        if (strcmp(msg->payload, "led=on") == 0) {
            gpio_clear(GPIOC, GPIO13); protocol_encode("ACK", "cmd=ok", response_buf, sizeof(response_buf));
            uart_tx_send(response_buf); return;
        } else if (strcmp(msg->payload, "led=off") == 0) {
            gpio_set(GPIOC, GPIO13); protocol_encode("ACK", "cmd=ok", response_buf, sizeof(response_buf));
            uart_tx_send(response_buf); return;
        } else if (strcmp(msg->payload, "led=toggle") == 0) {
            gpio_toggle(GPIOC, GPIO13); protocol_encode("ACK", "cmd=ok", response_buf, sizeof(response_buf));
            uart_tx_send(response_buf); return;
        }
    }

    // 3. Comando: SETEAR CÓDIGO MAESTRO DEL BUZÓN (Del MVP)
    if (strcmp(msg->type, "SET") == 0) {
        rx_count++;
        if (strlen(msg->payload) != LARGO_PIN) {
            protocol_encode("ERR", "code=invalid_pin_length", response_buf, sizeof(response_buf));
            uart_tx_send(response_buf);
            return;
        }

        int slot = -1;
        for (int i = 0; i < CANT_BUZONES; i++) {
            if (buzones_ocupados[i] == 0) { slot = i; break; }
        }

        if (slot != -1) {
            for (int i = 0; i < LARGO_PIN; i++) codigos_maestros[slot][i] = msg->payload[i] - '0';
            buzones_ocupados[slot] = 1;

            char payload_ok[32];
            snprintf(payload_ok, sizeof(payload_ok), "assigned_buzon=%d", slot + 1);
            protocol_encode("ACK", payload_ok, response_buf, sizeof(response_buf));
            uart_tx_send(response_buf);
            emitir_feedback(150000); // Feedback OK
        } else {
            protocol_encode("ERR", "code=all_mailboxes_full", response_buf, sizeof(response_buf));
            uart_tx_send(response_buf);
            emitir_feedback(700000); // Feedback Error
        }
        return;
    }

    // Comando Desconocido
    ae_count++;
    protocol_encode("ERR", "code=unknown_cmd", response_buf, sizeof(response_buf));
    uart_tx_send(response_buf);
}

// --- HARDWARE BASE LIBOPENCM3 (Adaptado al MVP) ---
void clock_setup(void) {
    rcc_clock_setup_pll(&rcc_hse_configs[RCC_CLOCK_HSE8_72MHZ]);
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    rcc_periph_clock_enable(RCC_AFIO);
    rcc_periph_clock_enable(RCC_TIM3);
    rcc_periph_clock_enable(RCC_ADC1);
    rcc_periph_clock_enable(RCC_USART1);
}

void gpio_setup(void) {
    // 5 Botones MVP (PA0 a PA4) -> Entradas con Pull-Up
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_PULL_UPDOWN, GPIO0|GPIO1|GPIO2|GPIO3|GPIO4);
    gpio_set(GPIOA, GPIO0|GPIO1|GPIO2|GPIO3|GPIO4);

    // ADC Canal 7 (PA7)
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_ANALOG, GPIO7); 

    // PWM Buzón 1 y 2 (PB0, PB1)
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO0 | GPIO1); 

    // Buzones 3,4,5 y Feedback (PB10, PB11, PB12, PB13, PB14)
    gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO10|GPIO11|GPIO12|GPIO13|GPIO14);
    gpio_clear(GPIOB, GPIO10|GPIO11|GPIO12|GPIO13|GPIO14);

    // USART (PA9 TX, PA10 RX)
    gpio_set_mode(GPIOA, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO9); 
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, GPIO10); 

    // LED de placa
    gpio_set_mode(GPIOC, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO13);
    gpio_set(GPIOC, GPIO13);
}

void usart_setup(void) {
    usart_set_baudrate(USART1, 115200);
    usart_set_databits(USART1, 8);
    usart_set_stopbits(USART1, USART_STOPBITS_1);
    usart_set_mode(USART1, USART_MODE_TX_RX);
    usart_set_parity(USART1, USART_PARITY_NONE);
    usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
    usart_enable(USART1);
}

void adc_setup(void) {
    rcc_set_adcpre(RCC_CFGR_ADCPRE_PCLK2_DIV6); 
    adc_power_off(ADC1);
    adc_set_regular_sequence(ADC1, 1, (uint8_t[]){7}); // Canal 7 = PA7
    adc_set_sample_time(ADC1, 7, ADC_SMPR_SMP_239DOT5CYC);
    adc_power_on(ADC1);
    for (volatile int i = 0; i < 8000; i++);
    adc_reset_calibration(ADC1);
    adc_calibrate(ADC1);
}

void tim3_setup(void) {
    rcc_periph_reset_pulse(RST_TIM3);
    timer_set_mode(TIM3, TIM_CR1_CKD_CK_INT, TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);
    timer_set_prescaler(TIM3, 72 - 1); // 1 MHz
    timer_set_period(TIM3, 1000 - 1);  // 1 kHz

    // Canal 3 (PB0) y Canal 4 (PB1) para PWM
    timer_disable_oc_output(TIM3, TIM_OC3);
    timer_set_oc_mode(TIM3, TIM_OC3, TIM_OCM_PWM1);
    timer_enable_oc_preload(TIM3, TIM_OC3);
    timer_set_oc_value(TIM3, TIM_OC3, 0);
    timer_enable_oc_output(TIM3, TIM_OC3);

    timer_disable_oc_output(TIM3, TIM_OC4);
    timer_set_oc_mode(TIM3, TIM_OC4, TIM_OCM_PWM1);
    timer_enable_oc_preload(TIM3, TIM_OC4);
    timer_set_oc_value(TIM3, TIM_OC4, 0);
    timer_enable_oc_output(TIM3, TIM_OC4);

    timer_enable_counter(TIM3);
}

// --- LOGICA DEL MVP ---
void emitir_feedback(uint32_t duracion) {
    gpio_set(GPIOB, GPIO13 | GPIO14); // Buzzer + LED
    for (volatile uint32_t j = 0; j < duracion; j++);
    gpio_clear(GPIOB, GPIO13 | GPIO14);
}

void procesar_pin(void) {
    int buzon_a_abrir = -1;
    for (int b = 0; b < CANT_BUZONES; b++) {
        uint8_t coincidencia = 1;
        for (int d = 0; d < LARGO_PIN; d++) {
            if (pin_usuario[d] != codigos_maestros[b][d]) { coincidencia = 0; break; }
        }
        if (coincidencia && buzones_ocupados[b] == 1) { buzon_a_abrir = b; break; }
    }

    if (buzon_a_abrir != -1) {
        emitir_feedback(100000); for (volatile int j = 0; j < 190000; j++); emitir_feedback(100000);
        abrir_buzon(buzon_a_abrir);
    } else {
        emitir_feedback(2000000);
    }
    digitos_ingresados = 0;
    for (int i = 0; i < LARGO_PIN; i++) pin_usuario[i] = 0;
}

void abrir_buzon(int num_buzon) {
    char mensaje_payload[32]; // Buffer temporal para armar el texto
    
    // snprintf convierte el entero en texto y lo mete en el buffer
    // num_buzon + 1 es para que muestre del 1 al 5 en lugar de 0 al 4
    snprintf(mensaje_payload, sizeof(mensaje_payload), "Buzon %d abierto", num_buzon + 1);

    // Ahora sí, pasás el buffer ya armado a protocol_encode
    protocol_encode("ACK", mensaje_payload, response_buf, sizeof(response_buf));
    uart_tx_send(response_buf);
    uart_tx_send("\r\n");
    // Buzones 1 y 2 con PWM interactivo (Canales 3 y 4 de TIM3)
    if (num_buzon == 0 || num_buzon == 1) {
        adc_start_conversion_direct(ADC1);
        while (!adc_eoc(ADC1)); 
        uint16_t adc = adc_read_regular(ADC1);
        uint32_t duty = (adc * 1000UL) / 4095UL;
        
        if (num_buzon == 0) timer_set_oc_value(TIM3, TIM_OC3, duty);
        if (num_buzon == 1) timer_set_oc_value(TIM3, TIM_OC4, duty);
        
        for (volatile int j = 0; j < 9000000; j++); // Espera apertura
        
        if (num_buzon == 0) timer_set_oc_value(TIM3, TIM_OC3, 0);
        if (num_buzon == 1) timer_set_oc_value(TIM3, TIM_OC4, 0);

    } 
    // Buzones 3, 4 y 5 (Digital)
    else {
        uint16_t target_pin = 0;
        if (num_buzon == 2) target_pin = GPIO10;
        if (num_buzon == 3) target_pin = GPIO11;
        if (num_buzon == 4) target_pin = GPIO12;
        
        gpio_set(GPIOB, target_pin);
        for (volatile int j = 0; j < 9000000; j++);
        gpio_clear(GPIOB, target_pin);
    }

    // Libera el buzón
    buzones_ocupados[num_buzon] = 0;
    for(int i = 0; i < LARGO_PIN; i++) codigos_maestros[num_buzon][i] = 0;
}