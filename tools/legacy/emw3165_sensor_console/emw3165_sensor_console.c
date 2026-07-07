#include <stdint.h>
#include <stddef.h>

#define HSI_HZ            16000000u
#define UART_BAUD         115200u
#define CONSOLE_LINE_MAX  192u
#define MAX_TOKENS        32u
#define SENSOR_PAYLOAD_MAX 512u
#define CONSOLE_TX_GAP_LOOPS 800u

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define REG16(addr) (*(volatile uint16_t *)(addr))

#define RCC_BASE      0x40023800u
#define RCC_CR        REG32(RCC_BASE + 0x00u)
#define RCC_CFGR      REG32(RCC_BASE + 0x08u)
#define RCC_AHB1ENR   REG32(RCC_BASE + 0x30u)
#define RCC_APB1ENR   REG32(RCC_BASE + 0x40u)
#define RCC_APB2ENR   REG32(RCC_BASE + 0x44u)
#define RCC_APB1RSTR  REG32(RCC_BASE + 0x20u)
#define RCC_APB2RSTR  REG32(RCC_BASE + 0x24u)

#define RCC_CR_HSION  (1u << 0)
#define RCC_CR_HSIRDY (1u << 1)
#define RCC_CR_PLLON  (1u << 24)
#define RCC_CFGR_SW   (3u << 0)
#define RCC_CFGR_SWS  (3u << 2)

#define GPIOA_BASE    0x40020000u
#define GPIOB_BASE    0x40020400u
#define GPIO_MODER(b)   REG32((b) + 0x00u)
#define GPIO_OTYPER(b)  REG32((b) + 0x04u)
#define GPIO_OSPEEDR(b) REG32((b) + 0x08u)
#define GPIO_PUPDR(b)   REG32((b) + 0x0cu)
#define GPIO_AFRL(b)    REG32((b) + 0x20u)
#define GPIO_AFRH(b)    REG32((b) + 0x24u)

#define USART1_BASE   0x40011000u
#define USART2_BASE   0x40004400u
#define USART_SR(b)   REG32((b) + 0x00u)
#define USART_DR(b)   REG32((b) + 0x04u)
#define USART_BRR(b)  REG32((b) + 0x08u)
#define USART_CR1(b)  REG32((b) + 0x0cu)
#define USART_CR2(b)  REG32((b) + 0x10u)
#define USART_CR3(b)  REG32((b) + 0x14u)

#define USART_SR_PE   (1u << 0)
#define USART_SR_FE   (1u << 1)
#define USART_SR_NE   (1u << 2)
#define USART_SR_ORE  (1u << 3)
#define USART_SR_RXNE (1u << 5)
#define USART_SR_TC   (1u << 6)
#define USART_SR_TXE  (1u << 7)
#define USART_SR_ERRS (USART_SR_PE | USART_SR_FE | USART_SR_NE | USART_SR_ORE)
#define USART_CR1_RE  (1u << 2)
#define USART_CR1_TE  (1u << 3)
#define USART_CR1_UE  (1u << 13)

#define SCB_VTOR      REG32(0xe000ed08u)
#define SCB_AIRCR     REG32(0xe000ed0cu)
#define SYST_CSR      REG32(0xe000e010u)

#define IWDG_BASE     0x40003000u
#define IWDG_KR       REG32(IWDG_BASE + 0x00u)

extern uint32_t __stack_top__;
extern uint32_t __data_load__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;

void Reset_Handler(void);
static void reset_main(void) __attribute__((used, noreturn));
static void Default_Handler(void) __attribute__((noreturn));

__attribute__((section(".isr_vector"), used))
static void (*const vector_table[])(void) = {
    (void (*)(void))&__stack_top__,
    Reset_Handler,
    Default_Handler,
    Default_Handler,
};

static char console_line[CONSOLE_LINE_MAX];
static uint32_t console_len;

struct sensor_rx {
    uint8_t state;
    uint32_t pos;
    uint32_t length;
    char cmd[4];
    uint8_t header[13];
    uint8_t payload[SENSOR_PAYLOAD_MAX];
};

static struct sensor_rx sensor_rx_state;
static uint32_t auto_score_enabled = 1u;

static void sensor_send_frame(const char cmd[4], char **fields, uint32_t field_count);

__attribute__((naked, section(".text.Reset_Handler")))
void Reset_Handler(void)
{
    __asm volatile (
        "ldr r0, =__stack_top__\n"
        "msr msp, r0\n"
        "b reset_main\n"
    );
}

static void Default_Handler(void)
{
    for (;;) {
        IWDG_KR = 0xaaaau;
    }
}

static void watchdog_kick(void)
{
    IWDG_KR = 0xaaaau;
}

static void delay(volatile uint32_t count)
{
    while (count-- != 0u) {
        if ((count & 0xfffu) == 0u) {
            watchdog_kick();
        }
        __asm volatile ("nop");
    }
}

static void memory_init(void)
{
    uint32_t *src = &__data_load__;
    uint32_t *dst = &__data_start__;
    while (dst < &__data_end__) {
        *dst++ = *src++;
    }

    dst = &__bss_start__;
    while (dst < &__bss_end__) {
        *dst++ = 0u;
    }
}

static void switch_to_hsi_16mhz(void)
{
    SYST_CSR = 0u;

    RCC_CR |= RCC_CR_HSION;
    while ((RCC_CR & RCC_CR_HSIRDY) == 0u) {
        watchdog_kick();
    }

    RCC_CFGR &= ~(RCC_CFGR_SW | (0xfu << 4) | (7u << 10) | (7u << 13));
    while ((RCC_CFGR & RCC_CFGR_SWS) != 0u) {
        watchdog_kick();
    }

    RCC_CR &= ~RCC_CR_PLLON;
}

static void gpio_set_mode(uint32_t base, uint32_t pin, uint32_t mode)
{
    const uint32_t shift = pin * 2u;
    GPIO_MODER(base) = (GPIO_MODER(base) & ~(3u << shift)) | (mode << shift);
}

static void gpio_set_af(uint32_t base, uint32_t pin, uint32_t af)
{
    const uint32_t shift = (pin & 7u) * 4u;
    if (pin < 8u) {
        GPIO_AFRL(base) = (GPIO_AFRL(base) & ~(0xfu << shift)) | (af << shift);
    } else {
        GPIO_AFRH(base) = (GPIO_AFRH(base) & ~(0xfu << shift)) | (af << shift);
    }
}

static void gpio_set_speed(uint32_t base, uint32_t pin, uint32_t speed)
{
    const uint32_t shift = pin * 2u;
    GPIO_OSPEEDR(base) = (GPIO_OSPEEDR(base) & ~(3u << shift)) | (speed << shift);
}

static void gpio_set_pupd(uint32_t base, uint32_t pin, uint32_t pupd)
{
    const uint32_t shift = pin * 2u;
    GPIO_PUPDR(base) = (GPIO_PUPDR(base) & ~(3u << shift)) | (pupd << shift);
}

static void uart_gpio_af(uint32_t port, uint32_t pin, uint32_t af, uint32_t pull)
{
    gpio_set_mode(port, pin, 2u);
    gpio_set_af(port, pin, af);
    gpio_set_speed(port, pin, 2u);
    gpio_set_pupd(port, pin, pull);
    GPIO_OTYPER(port) &= ~(1u << pin);
}

static void usart_common_init(uint32_t base)
{
    USART_CR1(base) = 0u;
    USART_CR2(base) = 0u;
    USART_CR3(base) = 0u;
    USART_BRR(base) = (HSI_HZ + (UART_BAUD / 2u)) / UART_BAUD;
    USART_CR1(base) = USART_CR1_RE | USART_CR1_TE | USART_CR1_UE;
}

static void console_uart_init(void)
{
    RCC_AHB1ENR |= (1u << 0) | (1u << 1);
    RCC_APB2ENR |= (1u << 4);
    (void)RCC_AHB1ENR;
    (void)RCC_APB2ENR;

    uart_gpio_af(GPIOB_BASE, 6u, 7u, 0u);
    uart_gpio_af(GPIOA_BASE, 10u, 7u, 1u);

    RCC_APB2RSTR |= (1u << 4);
    RCC_APB2RSTR &= ~(1u << 4);
    usart_common_init(USART1_BASE);
}

static void sensor_uart_init(void)
{
    RCC_AHB1ENR |= (1u << 0);
    RCC_APB1ENR |= (1u << 17);
    (void)RCC_AHB1ENR;
    (void)RCC_APB1ENR;

    uart_gpio_af(GPIOA_BASE, 2u, 7u, 0u);
    uart_gpio_af(GPIOA_BASE, 3u, 7u, 1u);

    RCC_APB1RSTR |= (1u << 17);
    RCC_APB1RSTR &= ~(1u << 17);
    usart_common_init(USART2_BASE);
}

static int usart_getc_nonblock(uint32_t base, uint8_t *out)
{
    const uint32_t sr = USART_SR(base);
    if ((sr & (USART_SR_RXNE | USART_SR_ERRS)) == 0u) {
        return 0;
    }

    *out = (uint8_t)USART_DR(base);
    return (sr & USART_SR_RXNE) != 0u;
}

static void usart_putc(uint32_t base, uint8_t value)
{
    while ((USART_SR(base) & USART_SR_TXE) == 0u) {
        watchdog_kick();
    }
    USART_DR(base) = value;
}

static void usart_flush(uint32_t base)
{
    while ((USART_SR(base) & USART_SR_TC) == 0u) {
        watchdog_kick();
    }
}

static void usart_write(uint32_t base, const void *data, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t i = 0; i < length; i++) {
        usart_putc(base, bytes[i]);
    }
}

static void console_putc(char c)
{
    if (c == '\n') {
        usart_putc(USART1_BASE, '\r');
        delay(CONSOLE_TX_GAP_LOOPS);
    }
    usart_putc(USART1_BASE, (uint8_t)c);
    delay(CONSOLE_TX_GAP_LOOPS);
}

static void console_write(const char *s)
{
    while (*s != '\0') {
        console_putc(*s++);
    }
}

static uint32_t cstr_len(const char *s)
{
    uint32_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int cstr_eq(const char *a, const char *b)
{
    while (*a == *b) {
        if (*a == '\0') {
            return 1;
        }
        a++;
        b++;
    }
    return 0;
}

static int cstr_eqn4(const char *a, const char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3] && a[4] == '\0';
}

static int cmd4_eq(const char cmd[4], const char *text)
{
    return cmd[0] == text[0] && cmd[1] == text[1] && cmd[2] == text[2] &&
           cmd[3] == text[3] && text[4] == '\0';
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

static uint8_t hex_nibble(uint8_t value)
{
    value &= 0xfu;
    if (value < 10u) {
        return (uint8_t)('0' + value);
    }
    return (uint8_t)('A' + value - 10u);
}

static int from_hex(char c, uint8_t *value)
{
    if (c >= '0' && c <= '9') {
        *value = (uint8_t)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *value = (uint8_t)(c - 'a' + 10);
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *value = (uint8_t)(c - 'A' + 10);
        return 1;
    }
    return 0;
}

static void console_hex8(uint8_t value)
{
    console_putc((char)hex_nibble(value >> 4));
    console_putc((char)hex_nibble(value));
}

static void console_hex32(uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4) {
        console_putc((char)hex_nibble((uint8_t)(value >> (uint32_t)shift)));
    }
}

static void console_dec(uint32_t value)
{
    char buf[10];
    uint32_t pos = 0u;
    if (value == 0u) {
        console_putc('0');
        return;
    }
    while (value != 0u && pos < sizeof(buf)) {
        buf[pos++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (pos != 0u) {
        console_putc(buf[--pos]);
    }
}

static void console_quoted(const uint8_t *data, uint32_t length)
{
    console_putc('"');
    for (uint32_t i = 0; i < length; i++) {
        const uint8_t c = data[i];
        if (c == '\\' || c == '"') {
            console_putc('\\');
            console_putc((char)c);
        } else if (c >= 32u && c < 127u) {
            console_putc((char)c);
        } else {
            console_write("\\x");
            console_hex8(c);
        }
    }
    console_putc('"');
}

static uint32_t decode_hex_len(const uint8_t *text, int *ok)
{
    uint32_t value = 0u;
    *ok = 1;
    for (uint32_t i = 0; i < 8u; i++) {
        uint8_t nibble = 0u;
        if (!from_hex((char)text[i], &nibble)) {
            *ok = 0;
            return 0u;
        }
        value = (value << 4) | nibble;
    }
    return value;
}

static void sensor_frame_reset(struct sensor_rx *rx)
{
    rx->state = 0u;
    rx->pos = 0u;
    rx->length = 0u;
}

static uint32_t payload_argc(const uint8_t *payload, uint32_t length)
{
    uint32_t argc = 0u;
    uint32_t start = 0u;
    while (start < length) {
        argc++;
        while (start < length && payload[start] != 0u) {
            start++;
        }
        start++;
    }
    return argc;
}

static const char *payload_next_field(const uint8_t *payload, uint32_t length, uint32_t *offset)
{
    const uint32_t start = *offset;
    if (start >= length) {
        return NULL;
    }

    while (*offset < length && payload[*offset] != 0u) {
        (*offset)++;
    }
    if (*offset >= length) {
        return NULL;
    }
    (*offset)++;
    return (const char *)&payload[start];
}

static int parse_fixed_centi(const char *text, int32_t *out)
{
    int negative = 0;
    uint32_t whole = 0u;
    uint32_t frac = 0u;
    uint32_t frac_digits = 0u;
    int saw_digit = 0;

    if (*text == '-') {
        negative = 1;
        text++;
    } else if (*text == '+') {
        text++;
    }

    while (*text >= '0' && *text <= '9') {
        saw_digit = 1;
        whole = (whole * 10u) + (uint32_t)(*text - '0');
        text++;
    }

    if (*text == '.') {
        text++;
        while (*text >= '0' && *text <= '9') {
            if (frac_digits < 2u) {
                frac = (frac * 10u) + (uint32_t)(*text - '0');
                frac_digits++;
            } else if (frac_digits == 2u && *text >= '5') {
                frac++;
                frac_digits++;
            }
            saw_digit = 1;
            text++;
        }
    }

    if (!saw_digit || *text != '\0') {
        return 0;
    }

    while (frac_digits < 2u) {
        frac *= 10u;
        frac_digits++;
    }
    if (frac >= 100u) {
        whole++;
        frac -= 100u;
    }

    int32_t value = (int32_t)((whole * 100u) + frac);
    if (negative) {
        value = -value;
    }
    *out = value;
    return 1;
}

static int32_t centi_to_int(int32_t centi)
{
    if (centi >= 0) {
        return (centi + 50) / 100;
    }
    return (centi - 50) / 100;
}

static uint32_t index_outside_range(int32_t value, int32_t low, int32_t high, int32_t step)
{
    int32_t delta = 0;
    if (value < low) {
        delta = low - value;
    } else if (value > high) {
        delta = value - high;
    }

    uint32_t index = 0u;
    if (delta > 0) {
        index = (uint32_t)((delta + step - 1) / step);
    }
    if (index > 4u) {
        index = 4u;
    }
    return index;
}

static uint32_t index_above(int32_t value, int32_t good_max, int32_t step)
{
    uint32_t index = 0u;
    if (value > good_max) {
        index = (uint32_t)((value - good_max + step - 1) / step);
    }
    if (index > 4u) {
        index = 4u;
    }
    return index;
}

static uint32_t penalty_outside_range(int32_t value, int32_t low, int32_t high,
                                      int32_t full_bad_delta, uint32_t max_penalty)
{
    int32_t delta = 0;
    if (value < low) {
        delta = low - value;
    } else if (value > high) {
        delta = value - high;
    }

    if (delta <= 0) {
        return 0u;
    }
    if (delta >= full_bad_delta) {
        return max_penalty;
    }
    return (uint32_t)(((uint32_t)delta * max_penalty + ((uint32_t)full_bad_delta / 2u)) /
                      (uint32_t)full_bad_delta);
}

static uint32_t penalty_above(int32_t value, int32_t good_max, int32_t full_bad_delta,
                              uint32_t max_penalty)
{
    if (value <= good_max) {
        return 0u;
    }

    const int32_t delta = value - good_max;
    if (delta >= full_bad_delta) {
        return max_penalty;
    }
    return (uint32_t)(((uint32_t)delta * max_penalty + ((uint32_t)full_bad_delta / 2u)) /
                      (uint32_t)full_bad_delta);
}

static void int32_to_cstr(int32_t value, char *out, uint32_t out_len)
{
    char tmp[11];
    uint32_t pos = 0u;
    uint32_t uvalue;

    if (out_len == 0u) {
        return;
    }

    if (value < 0) {
        *out++ = '-';
        out_len--;
        uvalue = (uint32_t)(-(value + 1)) + 1u;
    } else {
        uvalue = (uint32_t)value;
    }

    if (out_len == 0u) {
        return;
    }

    if (uvalue == 0u) {
        *out++ = '0';
        if (out_len > 1u) {
            *out = '\0';
        }
        return;
    }

    while (uvalue != 0u && pos < sizeof(tmp)) {
        tmp[pos++] = (char)('0' + (uvalue % 10u));
        uvalue /= 10u;
    }

    while (pos != 0u && out_len > 1u) {
        *out++ = tmp[--pos];
        out_len--;
    }
    *out = '\0';
}

struct sens_values {
    uint32_t seen;
    int32_t temp;
    int32_t humid;
    int32_t co2;
    int32_t voc;
    int32_t dust;
};

#define SENS_TEMP  (1u << 0)
#define SENS_HUMID (1u << 1)
#define SENS_CO2   (1u << 2)
#define SENS_VOC   (1u << 3)
#define SENS_DUST  (1u << 4)
#define SENS_ALL   (SENS_TEMP | SENS_HUMID | SENS_CO2 | SENS_VOC | SENS_DUST)

static void sens_parse_pair(struct sens_values *sens, const char *key, const char *value)
{
    int32_t parsed = 0;
    if (!parse_fixed_centi(value, &parsed)) {
        return;
    }

    if (cstr_eq(key, "temp")) {
        sens->temp = parsed;
        sens->seen |= SENS_TEMP;
    } else if (cstr_eq(key, "humid")) {
        sens->humid = parsed;
        sens->seen |= SENS_HUMID;
    } else if (cstr_eq(key, "co2")) {
        sens->co2 = parsed;
        sens->seen |= SENS_CO2;
    } else if (cstr_eq(key, "voc")) {
        sens->voc = parsed;
        sens->seen |= SENS_VOC;
    } else if (cstr_eq(key, "dust")) {
        sens->dust = parsed;
        sens->seen |= SENS_DUST;
    }
}

static char *score_color(uint32_t score)
{
    static char green[] = "green";
    static char amber[] = "amber";
    static char purple[] = "purple";

    if (score >= 80u) {
        return green;
    }
    if (score >= 60u) {
        return amber;
    }
    return purple;
}

static void send_scor_from_sens(const struct sens_values *sens)
{
    static char empty[] = "";
    char score_buf[12];
    char temp_index_buf[12];
    char humid_index_buf[12];
    char co2_index_buf[12];
    char voc_index_buf[12];
    char dust_index_buf[12];
    char temp_value_buf[12];
    char humid_value_buf[12];
    char co2_value_buf[12];
    char voc_value_buf[12];
    char dust_value_buf[12];

    const uint32_t temp_index = index_outside_range(sens->temp, 1800, 2600, 200);
    const uint32_t humid_index = index_outside_range(sens->humid, 3000, 6000, 1000);
    const uint32_t co2_index = index_above(sens->co2, 100000, 40000);
    const uint32_t voc_index = index_above(sens->voc, 33300, 33300);
    const uint32_t dust_index = index_above(sens->dust, 1200, 1200);

    uint32_t penalty = penalty_outside_range(sens->temp, 1800, 2600, 1200, 8u);
    penalty += penalty_outside_range(sens->humid, 3000, 6000, 4000, 8u);
    penalty += penalty_above(sens->co2, 100000, 80000, 25u);
    penalty += penalty_above(sens->voc, 33300, 100000, 25u);
    penalty += penalty_above(sens->dust, 1200, 6000, 34u);
    uint32_t score = 0u;
    if (penalty < 100u) {
        score = 100u - penalty;
    }

    int32_to_cstr((int32_t)score, score_buf, sizeof(score_buf));
    int32_to_cstr((int32_t)temp_index, temp_index_buf, sizeof(temp_index_buf));
    int32_to_cstr((int32_t)humid_index, humid_index_buf, sizeof(humid_index_buf));
    int32_to_cstr((int32_t)co2_index, co2_index_buf, sizeof(co2_index_buf));
    int32_to_cstr((int32_t)voc_index, voc_index_buf, sizeof(voc_index_buf));
    int32_to_cstr((int32_t)dust_index, dust_index_buf, sizeof(dust_index_buf));
    int32_to_cstr(centi_to_int(sens->temp), temp_value_buf, sizeof(temp_value_buf));
    int32_to_cstr(centi_to_int(sens->humid), humid_value_buf, sizeof(humid_value_buf));
    int32_to_cstr(centi_to_int(sens->co2), co2_value_buf, sizeof(co2_value_buf));
    int32_to_cstr(centi_to_int(sens->voc), voc_value_buf, sizeof(voc_value_buf));
    int32_to_cstr(centi_to_int(sens->dust), dust_value_buf, sizeof(dust_value_buf));

    char *color = score_color(score);
    char *fields[] = {
        "score", score_buf,
        "color", color,
        "index", empty,
        "temp", temp_index_buf,
        "humid", humid_index_buf,
        "co2", co2_index_buf,
        "voc", voc_index_buf,
        "dust", dust_index_buf,
        "sensor", empty,
        "temp", temp_value_buf,
        "humid", humid_value_buf,
        "co2", co2_value_buf,
        "voc", voc_value_buf,
        "dust", dust_value_buf,
    };

    sensor_send_frame("SCOR", fields, (uint32_t)(sizeof(fields) / sizeof(fields[0])));

    console_write("[auto] score=");
    console_write(score_buf);
    console_write(" color=");
    console_write(color);
    console_write(" t=");
    console_write(temp_value_buf);
    console_write(" h=");
    console_write(humid_value_buf);
    console_write(" co2=");
    console_write(co2_value_buf);
    console_write(" voc=");
    console_write(voc_value_buf);
    console_write(" dust=");
    console_write(dust_value_buf);
    console_putc('\n');
}

static void maybe_auto_score_sens(const struct sensor_rx *rx, uint8_t trailer)
{
    if (!auto_score_enabled || trailer != '#' || !cmd4_eq(rx->cmd, "SENS")) {
        return;
    }

    struct sens_values sens = {0u, 0, 0, 0, 0, 0};
    uint32_t offset = 0u;
    const char *key = payload_next_field(rx->payload, rx->length, &offset);
    while (key != NULL) {
        const char *value = payload_next_field(rx->payload, rx->length, &offset);
        if (value == NULL) {
            break;
        }
        sens_parse_pair(&sens, key, value);
        key = payload_next_field(rx->payload, rx->length, &offset);
    }

    if ((sens.seen & SENS_ALL) != SENS_ALL) {
        console_write("\n[auto] incomplete SENS mask=0x");
        console_hex32(sens.seen);
        console_write("\n> ");
        return;
    }

    send_scor_from_sens(&sens);
}

static void print_sensor_frame(const struct sensor_rx *rx, uint8_t trailer)
{
    const uint32_t argc = payload_argc(rx->payload, rx->length);
    console_write("\n[rx] ");
    for (uint32_t i = 0; i < 4u; i++) {
        console_putc(rx->cmd[i]);
    }
    console_write(" len=");
    console_dec(rx->length);
    console_write(" argc=");
    console_dec(argc);
    console_write(" trailer=0x");
    console_hex8(trailer);
    console_putc('\n');

    uint32_t arg = 0u;
    uint32_t start = 0u;
    while (start < rx->length) {
        uint32_t end = start;
        while (end < rx->length && rx->payload[end] != 0u) {
            end++;
        }
        console_write("  [");
        console_dec(arg++);
        console_write("] ");
        console_quoted(&rx->payload[start], end - start);
        console_putc('\n');
        start = end + 1u;
    }
    console_write("> ");
}

static void sensor_rx_byte(struct sensor_rx *rx, uint8_t value)
{
    int ok = 0;

    switch (rx->state) {
    case 0:
        if (value == '*') {
            rx->state = 1u;
            rx->pos = 0u;
        } else {
            console_write("\n[rx stray] 0x");
            console_hex8(value);
            console_write("\n> ");
        }
        break;

    case 1:
        rx->header[rx->pos++] = value;
        if (rx->pos == sizeof(rx->header)) {
            for (uint32_t i = 0; i < 4u; i++) {
                rx->cmd[i] = (char)rx->header[i];
            }
            rx->length = decode_hex_len(&rx->header[4], &ok);
            if (!ok || rx->length > SENSOR_PAYLOAD_MAX) {
                console_write("\n[rx bad header] cmd=");
                for (uint32_t i = 0; i < 4u; i++) {
                    console_putc(rx->cmd[i]);
                }
                console_write(" len=0x");
                console_hex32(rx->length);
                console_write("\n> ");
                sensor_frame_reset(rx);
            } else if (rx->length == 0u) {
                rx->state = 3u;
                rx->pos = 0u;
            } else {
                rx->state = 2u;
                rx->pos = 0u;
            }
        }
        break;

    case 2:
        rx->payload[rx->pos++] = value;
        if (rx->pos == rx->length) {
            rx->state = 3u;
            rx->pos = 0u;
        }
        break;

    case 3:
        maybe_auto_score_sens(rx, value);
        print_sensor_frame(rx, value);
        sensor_frame_reset(rx);
        break;

    default:
        sensor_frame_reset(rx);
        break;
    }
}

static void sensor_poll(void)
{
    uint8_t value = 0u;
    while (usart_getc_nonblock(USART2_BASE, &value)) {
        sensor_rx_byte(&sensor_rx_state, value);
    }
}

static void frame_len_hex(uint32_t value, char out[9])
{
    for (int shift = 28; shift >= 0; shift -= 4) {
        *out++ = (char)hex_nibble((uint8_t)(value >> (uint32_t)shift));
    }
    *out = '\0';
}

static uint32_t fields_payload_len(char **fields, uint32_t count)
{
    uint32_t length = 0u;
    for (uint32_t i = 0; i < count; i++) {
        length += cstr_len(fields[i]) + 1u;
    }
    return length;
}

static void sensor_send_frame(const char cmd[4], char **fields, uint32_t field_count)
{
    const uint32_t payload_len = fields_payload_len(fields, field_count);
    char lenbuf[9];
    frame_len_hex(payload_len, lenbuf);

    usart_putc(USART2_BASE, '*');
    usart_write(USART2_BASE, cmd, 4u);
    usart_write(USART2_BASE, lenbuf, sizeof(lenbuf));
    for (uint32_t i = 0; i < field_count; i++) {
        usart_write(USART2_BASE, fields[i], cstr_len(fields[i]));
        usart_putc(USART2_BASE, 0u);
    }
    usart_putc(USART2_BASE, '#');
    usart_flush(USART2_BASE);

    console_write("[tx] ");
    for (uint32_t i = 0; i < 4u; i++) {
        console_putc(cmd[i]);
    }
    console_write(" len=");
    console_dec(payload_len);
    console_write(" fields=");
    console_dec(field_count);
    console_putc('\n');
}

static void sensor_send_raw(const uint8_t *bytes, uint32_t count)
{
    usart_write(USART2_BASE, bytes, count);
    usart_flush(USART2_BASE);
    console_write("[tx raw] ");
    console_dec(count);
    console_write(" bytes\n");
}

static uint32_t tokenize(char *line, char **tokens, uint32_t max_tokens)
{
    uint32_t count = 0u;
    char *p = line;

    while (*p != '\0') {
        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (count == max_tokens) {
            break;
        }

        char *dst = p;
        tokens[count++] = dst;
        int quoted = 0;
        if (*p == '"') {
            quoted = 1;
            p++;
        }

        while (*p != '\0') {
            if (quoted) {
                if (*p == '"') {
                    p++;
                    break;
                }
                if (*p == '\\' && p[1] != '\0') {
                    p++;
                }
                *dst++ = *p++;
            } else {
                if (is_space(*p)) {
                    break;
                }
                *dst++ = *p++;
            }
        }

        const int stopped_on_space = is_space(*p);
        *dst = '\0';
        if (stopped_on_space) {
            p++;
        }
        while (is_space(*p)) {
            p++;
        }
    }

    return count;
}

static void cmd_help(void)
{
    console_write("commands:\n");
    console_write("  help\n");
    console_write("  frame CMD [field ...]\n");
    console_write("  score VALUE [color]\n");
    console_write("  autoscore on|off\n");
    console_write("  net up|down\n");
    console_write("  time YYYYMMDDhhmmss\n");
    console_write("  raw HEXBYTES\n");
    console_write("  reset\n");
}

static void cmd_autoscore(char **tokens, uint32_t count)
{
    if (count < 2u) {
        console_write("autoscore ");
        console_write(auto_score_enabled ? "on\n" : "off\n");
        return;
    }
    if (cstr_eq(tokens[1], "on")) {
        auto_score_enabled = 1u;
        console_write("autoscore on\n");
    } else if (cstr_eq(tokens[1], "off")) {
        auto_score_enabled = 0u;
        console_write("autoscore off\n");
    } else {
        console_write("usage: autoscore on|off\n");
    }
}

static void cmd_frame(char **tokens, uint32_t count)
{
    if (count < 2u || cstr_len(tokens[1]) != 4u) {
        console_write("usage: frame CMD [field ...]\n");
        return;
    }
    sensor_send_frame(tokens[1], &tokens[2], count - 2u);
}

static void cmd_net(char **tokens, uint32_t count)
{
    static char *up_fields[] = {
        "net", "1",
        "rssi", "-40",
        "ip", "192.168.4.1",
        "mac", "70:88:6B:10:48:3B",
    };
    static char *down_fields[] = {
        "net", "0",
        "rssi", "0",
        "ip", "0.0.0.0",
        "mac", "70:88:6B:10:48:3B",
    };

    if (count < 2u) {
        console_write("usage: net up|down\n");
        return;
    }
    if (cstr_eq(tokens[1], "up")) {
        sensor_send_frame("NETW", up_fields, (uint32_t)(sizeof(up_fields) / sizeof(up_fields[0])));
    } else if (cstr_eq(tokens[1], "down")) {
        sensor_send_frame("NETW", down_fields, (uint32_t)(sizeof(down_fields) / sizeof(down_fields[0])));
    } else {
        console_write("usage: net up|down\n");
    }
}

static void cmd_score(char **tokens, uint32_t count)
{
    static char empty[] = "";
    static char zero[] = "0";
    static char default_color[] = "green";
    char *color = default_color;

    if (count < 2u) {
        console_write("usage: score VALUE [color]\n");
        return;
    }
    if (count >= 3u) {
        color = tokens[2];
    }

    char *fields[] = {
        "score", tokens[1],
        "color", color,
        "index", empty,
        "temp", zero,
        "humid", zero,
        "co2", zero,
        "voc", zero,
        "dust", zero,
        "sensor", empty,
        "temp", zero,
        "humid", zero,
        "co2", zero,
        "voc", zero,
        "dust", zero,
    };

    sensor_send_frame("SCOR", fields, (uint32_t)(sizeof(fields) / sizeof(fields[0])));
}

static void cmd_time(char **tokens, uint32_t count)
{
    char *fields[2];
    if (count < 2u) {
        console_write("usage: time YYYYMMDDhhmmss\n");
        return;
    }
    fields[0] = "time";
    fields[1] = tokens[1];
    sensor_send_frame("TIME", fields, 2u);
}

static void cmd_raw(char **tokens, uint32_t count)
{
    uint8_t bytes[96];
    uint32_t out = 0u;
    uint8_t high = 0u;
    int have_high = 0;

    if (count < 2u) {
        console_write("usage: raw HEXBYTES\n");
        return;
    }

    for (uint32_t t = 1u; t < count; t++) {
        const char *p = tokens[t];
        while (*p != '\0') {
            uint8_t nibble = 0u;
            if (!from_hex(*p++, &nibble)) {
                console_write("raw: non-hex character\n");
                return;
            }
            if (!have_high) {
                high = nibble;
                have_high = 1;
            } else {
                if (out == sizeof(bytes)) {
                    console_write("raw: too many bytes\n");
                    return;
                }
                bytes[out++] = (uint8_t)((high << 4) | nibble);
                have_high = 0;
            }
        }
    }

    if (have_high) {
        console_write("raw: odd number of hex nibbles\n");
        return;
    }
    sensor_send_raw(bytes, out);
}

static void system_reset(void)
{
    usart_flush(USART1_BASE);
    SCB_AIRCR = 0x05fa0004u;
    for (;;) {
        watchdog_kick();
    }
}

static void run_command(char *line)
{
    char *tokens[MAX_TOKENS];
    const uint32_t count = tokenize(line, tokens, MAX_TOKENS);

    if (count == 0u) {
        return;
    }
    if (cstr_eq(tokens[0], "help") || cstr_eq(tokens[0], "?")) {
        cmd_help();
    } else if (cstr_eq(tokens[0], "frame") || cstr_eq(tokens[0], "tx")) {
        cmd_frame(tokens, count);
    } else if (cstr_eq(tokens[0], "score")) {
        cmd_score(tokens, count);
    } else if (cstr_eq(tokens[0], "autoscore")) {
        cmd_autoscore(tokens, count);
    } else if (cstr_eq(tokens[0], "net")) {
        cmd_net(tokens, count);
    } else if (cstr_eq(tokens[0], "time")) {
        cmd_time(tokens, count);
    } else if (cstr_eq(tokens[0], "raw")) {
        cmd_raw(tokens, count);
    } else if (cstr_eq(tokens[0], "reset")) {
        console_write("resetting\n");
        system_reset();
    } else if (cstr_eqn4(tokens[0], "WVER") || cstr_eqn4(tokens[0], "SVER") ||
               cstr_eqn4(tokens[0], "DVID") || cstr_eqn4(tokens[0], "MACA")) {
        sensor_send_frame(tokens[0], &tokens[1], count - 1u);
    } else {
        console_write("unknown command; type help\n");
    }
}

static void console_rx_byte(uint8_t value)
{
    if (value == '\r' || value == '\n') {
        console_putc('\n');
        console_line[console_len] = '\0';
        run_command(console_line);
        console_len = 0u;
        console_write("> ");
        return;
    }

    if (value == 0x08u || value == 0x7fu) {
        if (console_len != 0u) {
            console_len--;
            console_write("\b \b");
        }
        return;
    }

    if (value >= 32u && value < 127u) {
        if (console_len + 1u < CONSOLE_LINE_MAX) {
            console_line[console_len++] = (char)value;
            console_putc((char)value);
        }
    }
}

static void console_poll(void)
{
    uint8_t value = 0u;
    while (usart_getc_nonblock(USART1_BASE, &value)) {
        console_rx_byte(value);
    }
}

static void reset_main(void)
{
    __asm volatile ("cpsid i");
    SCB_VTOR = 0x08000000u;

    memory_init();
    switch_to_hsi_16mhz();
    console_uart_init();
    sensor_uart_init();
    sensor_frame_reset(&sensor_rx_state);

    delay(100000u);
    console_write("\nAwair EMW3165 F411 sensor console\n");
    console_write("console: USART1 PB6 TX / PA10 RX, 115200 8N1\n");
    console_write("sensor:  USART2 PA2 TX / PA3 RX, 115200 8N1\n");
    console_write("type help\n> ");

    for (;;) {
        watchdog_kick();
        sensor_poll();
        console_poll();
    }
}
