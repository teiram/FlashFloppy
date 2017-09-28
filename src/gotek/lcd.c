/*
 * lcd.c
 * 
 * 1. HD44780 LCD controller via a PCF8574 I2C backpack.
 * 2. SSD1306 OLED controller driving 128x32 bitmap display.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* PCF8574 pin assignment: D7-D6-D5-D4-BL-EN-RW-RS */
#define _D7 (1u<<7)
#define _D6 (1u<<6)
#define _D5 (1u<<5)
#define _D4 (1u<<4)
#define _BL (1u<<3)
#define _EN (1u<<2)
#define _RW (1u<<1)
#define _RS (1u<<0)

/* HD44780 commands */
#define CMD_ENTRYMODE    0x04
#define CMD_DISPLAYCTL   0x08
#define CMD_DISPLAYSHIFT 0x10
#define CMD_FUNCTIONSET  0x20
#define CMD_SETCGRADDR   0x40
#define CMD_SETDDRADDR   0x80
#define FS_2LINE         0x08

/* STM32 I2C peripheral. */
#define i2c i2c2
#define SCL 10
#define SDA 11

/* I2C error ISR. */
#define I2C_ERROR_IRQ 34
void IRQ_34(void) __attribute__((alias("IRQ_i2c_error")));

/* I2C event ISR. */
#define I2C_EVENT_IRQ 33
void IRQ_33(void) __attribute__((alias("IRQ_i2c_event")));

/* DMA completion ISR. */
#define DMA1_CH4_IRQ 14
void IRQ_14(void) __attribute__((alias("IRQ_dma1_ch4_tc")));

static uint8_t _bl;
static uint8_t i2c_addr;
static uint8_t i2c_dead;

#define OLED_ADDR 0x3c
static void oled_init(void);
static unsigned int oled_prep_buffer(void);

/* Count of display-refresh completions. For synchronisation/flush. */
static volatile uint8_t refresh_count;

/* I2C data buffer. Data is DMAed to the I2C peripheral. */
static uint32_t buffer[256/4];

/* 16x2 text buffer, rendered into I2C data and placed into buffer[]. */
static char text[2][16];

/* Occasionally the I2C/DMA engine seems to get stuck. Detect this with 
 * a timeout timer and unwedge it by calling the I2C error handler. */
#define DMA_TIMEOUT stk_ms(200)
static struct timer timeout_timer;
static void timeout_fn(void *unused)
{
    IRQx_set_pending(I2C_ERROR_IRQ);
}

/* I2C Error ISR: Reset the peripheral and reinit everything. */
static void IRQ_i2c_error(void)
{
    /* Dump and clear I2C errors. */
    printk("I2C: Error (%04x)\n", (uint16_t)(i2c->sr1 & I2C_SR1_ERRORS));
    i2c->sr1 &= ~I2C_SR1_ERRORS;

    /* Clear the I2C peripheral. */
    i2c->cr1 = 0;
    i2c->cr1 = I2C_CR1_SWRST;

    /* Clear the DMA controller. */
    dma1->ch4.ccr = 0;
    dma1->ifcr = DMA_IFCR_CGIF(4);

    timer_cancel(&timeout_timer);

    lcd_init();
}

static void IRQ_i2c_event(void)
{
    uint16_t sr1 = i2c->sr1;

    if (sr1 & I2C_SR1_SB) {
        /* Send address. Clears SR1_SB. */
        i2c->dr = i2c_addr << 1;
    }

    if (sr1 & I2C_SR1_ADDR) {
        /* Read SR2 clears SR1_ADDR. */
        (void)i2c->sr2;
        /* No more events: data phase is driven by DMA. */
        i2c->cr2 &= ~I2C_CR2_ITEVTEN;
    }
}

/* Start an I2C DMA sequence. */
static void dma_start(unsigned int sz)
{
    ASSERT(sz <= sizeof(buffer));

    dma1->ch4.cmar = (uint32_t)(unsigned long)buffer;
    dma1->ch4.cndtr = sz;
    dma1->ch4.ccr = (DMA_CCR_MSIZE_8BIT |
                     DMA_CCR_PSIZE_16BIT |
                     DMA_CCR_MINC |
                     DMA_CCR_DIR_M2P |
                     DMA_CCR_TCIE |
                     DMA_CCR_EN);

    /* Set the timeout timer in case the DMA hangs for any reason. */
    timer_set(&timeout_timer, stk_add(stk_now(), DMA_TIMEOUT));
}

/* Emit a 4-bit command to the HD44780 via the DMA buffer. */
static void emit4(uint8_t **p, uint8_t val)
{
    *(*p)++ = val;
    *(*p)++ = val | _EN;
    *(*p)++ = val;
}

/* Emit an 8-bit command to the HD44780 via the DMA buffer. */
static void emit8(uint8_t **p, uint8_t val, uint8_t signals)
{
    signals |= _bl;
    emit4(p, (val & 0xf0) | signals);
    emit4(p, (val << 4) | signals);
}

/* Snapshot text buffer into the command buffer. */
static unsigned int lcd_prep_buffer(void)
{
    uint8_t *q = (uint8_t *)buffer;
    unsigned int i, j;

    /* We transmit complete display on every DMA. */
    refresh_count++;

    for (i = 0; i < 2; i++) {
        emit8(&q, CMD_SETDDRADDR | (i*64), 0);
        for (j = 0; j < 16; j++)
            emit8(&q, text[i][j], _RS);
    }

    return q - (uint8_t *)buffer;
}

static void IRQ_dma1_ch4_tc(void)
{
    unsigned int dma_sz;

    /* Clear the DMA controller. */
    dma1->ch4.ccr = 0;
    dma1->ifcr = DMA_IFCR_CGIF(4);

    /* Prepare the DMA buffer and start the next DMA sequence. */
    dma_sz = (i2c_addr == OLED_ADDR) ? oled_prep_buffer() : lcd_prep_buffer();
    dma_start(dma_sz);
}

/* Wait for given status condition @s while also checking for errors. */
static bool_t i2c_wait(uint8_t s)
{
    stk_time_t t = stk_now();
    while ((i2c->sr1 & s) != s) {
        if (i2c->sr1 & I2C_SR1_ERRORS) {
            i2c->sr1 &= ~I2C_SR1_ERRORS;
            return FALSE;
        }
        if (stk_diff(t, stk_now()) > stk_ms(10)) {
            /* I2C bus seems to be locked up. */
            i2c_dead = TRUE;
            return FALSE;
        }
    }
    return TRUE;
}

/* Synchronously transmit the I2C START sequence. */
static bool_t i2c_start(uint8_t a)
{
    i2c->cr1 |= I2C_CR1_START;
    if (!i2c_wait(I2C_SR1_SB))
        return FALSE;
    i2c->dr = a << 1;
    if (!i2c_wait(I2C_SR1_ADDR))
        return FALSE;
    (void)i2c->sr2;
    return TRUE;
}

/* Synchronously transmit an I2C command. */
static bool_t i2c_cmd(uint8_t cmd)
{
    i2c->dr = cmd;
    return i2c_wait(I2C_SR1_BTF);
}

/* Write a 4-bit nibble over D7-D4 (4-bit bus). */
static void write4(uint8_t val)
{
    i2c_cmd(val);
    i2c_cmd(val | _EN);
    i2c_cmd(val);
}

/* Check whether an I2C device is responding at given address. */
static bool_t i2c_probe(uint8_t a)
{
    if (!i2c_start(a) || !i2c_cmd(0))
        return FALSE;
    i2c->cr1 |= I2C_CR1_STOP;
    while (i2c->cr1 & I2C_CR1_STOP)
        continue;
    return TRUE;
}

/* Check given inclusive range of addresses for a responding I2C device. */
static uint8_t i2c_probe_range(uint8_t s, uint8_t e)
{
    uint8_t a;
    for (a = s; (a <= e) && !i2c_dead; a++)
        if (i2c_probe(a))
            return a;
    return 0;
}

void lcd_clear(void)
{
    lcd_write(0, 0, 16, "");
    lcd_write(0, 1, 16, "");
}

void lcd_write(int col, int row, int min, const char *str)
{
    char c, *p = &text[row][col];
    uint32_t oldpri;

    /* Prevent the text[] getting rendered while we're updating it. */
    oldpri = IRQ_save(I2C_IRQ_PRI);

    while ((c = *str++) && (col++ < 16)) {
        *p++ = c;
        min--;
    }
    while ((min-- > 0) && (col++ < 16))
        *p++ = ' ';

    IRQ_restore(oldpri);
}

void lcd_backlight(bool_t on)
{
    /* Will be picked up the next time text[] is rendered. */
    _bl = on ? _BL : 0;
}

void lcd_sync(void)
{
    uint8_t c = refresh_count;
    while ((uint8_t)(refresh_count - c) < 2)
        cpu_relax();
}

bool_t lcd_init(void)
{
    uint8_t a, *p;
    bool_t reinit = (i2c_addr != 0);

    rcc->apb1enr |= RCC_APB1ENR_I2C2EN;

    /* Check we have a clear I2C bus. Both clock and data must be high. If SDA 
     * is stuck low then slave may be stuck in an ACK cycle. We can try to 
     * unwedge the slave in that case and drive it into the STOP condition. */
    gpio_configure_pin(gpiob, SCL, GPO_opendrain(_2MHz, HIGH));
    gpio_configure_pin(gpiob, SDA, GPO_opendrain(_2MHz, HIGH));
    delay_us(10);
    if (gpio_read_pin(gpiob, SCL) && !gpio_read_pin(gpiob, SDA)) {
        printk("I2C: SDA held by slave? Fixing... ");
        /* We will hold SDA low (as slave is) and also drive SCL low to end 
         * the current ACK cycle. */
        gpio_write_pin(gpiob, SDA, FALSE);
        gpio_write_pin(gpiob, SCL, FALSE);
        delay_us(10);
        /* Slave should no longer be driving SDA low (but we still are).
         * Now prepare for the STOP condition by setting SCL high. */
        gpio_write_pin(gpiob, SCL, TRUE);
        delay_us(10);
        /* Enter the STOP condition by setting SDA high while SCL is high. */
        gpio_write_pin(gpiob, SDA, TRUE);
        delay_us(10);
        printk("%s\n",
               !gpio_read_pin(gpiob, SCL) || !gpio_read_pin(gpiob, SDA)
               ? "Still held" : "Done");
    }

    /* Check the bus is not floating (or still stuck!). We shouldn't be able to 
     * pull the lines low with our internal weak pull-downs (min. 30kohm). */
    if (!reinit) {
        gpio_configure_pin(gpiob, SCL, GPI_pull_down);
        gpio_configure_pin(gpiob, SDA, GPI_pull_down);
        delay_us(10);
        if (!gpio_read_pin(gpiob, SCL) || !gpio_read_pin(gpiob, SDA)) {
            printk("I2C: Invalid bus\n");
            goto fail;
        }
    }

    gpio_configure_pin(gpiob, SCL, AFO_opendrain(_2MHz));
    gpio_configure_pin(gpiob, SDA, AFO_opendrain(_2MHz));

    /* Standard Mode (100kHz) */
    i2c->cr1 = 0;
    i2c->cr2 = I2C_CR2_FREQ(36);
    i2c->ccr = I2C_CCR_CCR(180);
    i2c->trise = 37;
    i2c->cr1 = I2C_CR1_PE;

    if (!reinit) {
        /* Probe the bus for an I2C device. */
        a = i2c_probe_range(0x20, 0x27) ?: i2c_probe_range(0x38, 0x3f);
        if (a == 0) {
            printk("I2C: %s\n",
                   i2c_dead ? "Bus locked up?" : "No device found");
            goto fail;
        }

        printk("I2C: %s found at 0x%02x\n",
               (a == OLED_ADDR) ? "OLED" : "LCD", a);
        i2c_addr = a;

        lcd_clear();
    }

    /* Enable the Event IRQ. */
    IRQx_set_prio(I2C_EVENT_IRQ, I2C_IRQ_PRI);
    IRQx_clear_pending(I2C_EVENT_IRQ);
    IRQx_enable(I2C_EVENT_IRQ);

    /* Enable the Error IRQ. */
    IRQx_set_prio(I2C_ERROR_IRQ, I2C_IRQ_PRI);
    IRQx_clear_pending(I2C_ERROR_IRQ);
    IRQx_enable(I2C_ERROR_IRQ);
    i2c->cr2 |= I2C_CR2_ITERREN;

    /* Initialise DMA1 channel 4 and its completion interrupt. */
    dma1->ch4.cpar = (uint32_t)(unsigned long)&i2c->dr;
    dma1->ifcr = DMA_IFCR_CGIF(4);
    IRQx_set_prio(DMA1_CH4_IRQ, I2C_IRQ_PRI);
    IRQx_clear_pending(DMA1_CH4_IRQ);
    IRQx_enable(DMA1_CH4_IRQ);

    /* Timeout handler for if I2C transmission borks. */
    timer_init(&timeout_timer, timeout_fn, NULL);
    timer_set(&timeout_timer, stk_add(stk_now(), DMA_TIMEOUT));

    if (i2c_addr == OLED_ADDR) {
        oled_init();
        return TRUE;
    }

    if (!i2c_start(i2c_addr))
        goto fail;

    /* Initialise 4-bit interface, as in the datasheet. Do this synchronously
     * and with the required delays. */
    write4(3 << 4);
    delay_us(4100);
    write4(3 << 4);
    delay_us(100);
    write4(3 << 4);
    write4(2 << 4);

    /* More initialisation from the datasheet. Send by DMA. */
    p = (uint8_t *)buffer;
    emit8(&p, CMD_FUNCTIONSET | FS_2LINE, 0);
    emit8(&p, CMD_DISPLAYCTL, 0);
    emit8(&p, CMD_ENTRYMODE | 2, 0);
    emit8(&p, CMD_DISPLAYCTL | 4, 0); /* display on */
    i2c->cr2 |= I2C_CR2_DMAEN;
    dma_start(p - (uint8_t *)buffer);
    
    /* Wait for DMA engine to initialise RAM, then turn on backlight. */
    if (!reinit) {
        lcd_sync();
        lcd_backlight(TRUE);
    }

    return TRUE;

fail:
    if (reinit)
        return FALSE;
    IRQx_disable(I2C_ERROR_IRQ);
    IRQx_disable(DMA1_CH4_IRQ);
    i2c->cr1 &= ~I2C_CR1_PE;
    gpio_configure_pin(gpiob, SCL, GPI_pull_up);
    gpio_configure_pin(gpiob, SDA, GPI_pull_up);
    rcc->apb1enr &= ~RCC_APB1ENR_I2C2EN;
    return FALSE;
}

#ifdef font_7x16

extern const uint8_t oled_font_7x16[];

static void oled_convert_text_row(char *pc)
{
    unsigned int i, c;
    const uint8_t *p;
    uint8_t *q = (uint8_t *)buffer;

    for (i = 0; i < 16; i++) {
        if ((c = *pc++ - 0x20) > 0x5e)
            c = '.' - 0x20;
        p = &oled_font_7x16[c * 14];
        memcpy(q, p, 7);
        memcpy(q+128, p+7, 7);
        q += 7;
    }

    /* Fill remainder of buffer[] with zeroes. */
    memset(q, 0, 16);
    memset(q+128, 0, 16);
}

#else

extern const uint32_t oled_font_8x16[];

static void oled_convert_text_row(char *pc)
{
    unsigned int i, c;
    const uint32_t *p;
    uint32_t *q = buffer;

    for (i = 0; i < 16; i++) {
        if ((c = *pc++ - 0x20) > 0x5e)
            c = '.' - 0x20;
        p = &oled_font_8x16[c * 4];
        *q++ = *p++;
        *q++ = *p++;
        q[32-2] = *p++;
        q[32-1] = *p++;
    }
}

#endif

static uint8_t oled_row;

static unsigned int oled_start_i2c(uint8_t *buf)
{
    static const uint8_t setup_addr_cmds[] = {
        0x20, 0,      /* horizontal addressing mode */
        0x21, 0, 127, /* column address range: 0-127 */
        0x22, 0, 3    /* page address range: 0-3 */
    };

    uint8_t *p = buf;
    int i;

    /* Set up the display address range. */
    for (i = 0; i < sizeof(setup_addr_cmds); i++) {
        *p++ = 0x80; /* Co=1, Command */
        *p++ = setup_addr_cmds[i];
    }

    /* Display on/off according to backlight setting. */
    *p++ = 0x80;
    *p++ = _bl ? 0xaf : 0xae;

    /* All subsequent bytes are data bytes. */
    *p++ = 0x40;
    oled_row = 0;

    /* Start the I2C transaction. */
    i2c->cr2 |= I2C_CR2_ITEVTEN;
    i2c->cr1 |= I2C_CR1_START;

    return p - buf;
}

/* Snapshot text buffer into the bitmap buffer. */
static unsigned int oled_prep_buffer(void)
{
    /* If we have completed a complete fill of the OLED display, start a new 
     * I2C transaction. The OLED display seems to occasionally silently lose 
     * a byte and then we lose sync with the display address. */
    if (oled_row == 2) {
        refresh_count++;
        /* Wait for BTF. */
        while (!(i2c->sr1 & I2C_SR1_BTF)) {
            /* Any errors: bail and leave it to the Error ISR. */
            if (i2c->sr1 & I2C_SR1_ERRORS)
                return 0;
        }
        /* Send STOP. Clears SR1_TXE and SR1_BTF. */
        i2c->cr1 |= I2C_CR1_STOP;
        while (i2c->cr1 & I2C_CR1_STOP)
            continue;
        /* Kick off new I2C transaction. */
        return oled_start_i2c((uint8_t *)buffer);
    }

    /* Convert one row of text[] into buffer[] writes. */
    oled_convert_text_row(text[oled_row++]);

    return 256;
}

static void oled_init(void)
{
    static const uint8_t init_cmds[] = {
        0xd5, 0x80, /* default clock */
        0xa8, 31,   /* multiplex ratio (lcd height - 1) */
        0xd3, 0x00, /* display offset = 0 */
        0x40,       /* display start line = 0 */
        0x8d, 0x14, /* enable charge pump */
        0xa1,       /* segment mapping (reverse) */
        0xc8,       /* com scan direction (decrement) */
        0xda, 0x02, /* com pins configuration */
        0x81, 0x8f, /* display contrast */
        0xd9, 0xf1, /* pre-charge period */
        0xdb, 0x20, /* vcomh detect (default) */
        0xa4,       /* output follows ram contents */
        0xa6,       /* normal display output (inverse=off) */
        0x2e,       /* deactivate scroll */
    };
    /* NB. Changes for 128x64 display: 0xa8, 63, 0xda, 0x12, 0x81, 0xcf. 
     * Otherwise above settings create a double-height 128x32 viewport
     * utilising alternate display lines (which is a sane fallback). 
     * NB. 128x64 displays may have I2C address 0x3c same as 128x32 display. */

    uint8_t *p = (uint8_t *)buffer;
    int i;

    /* Disable I2C (currently in Standard Mode). */
    i2c->cr1 = 0;

    /* Fast Mode (400kHz). */
    i2c->cr2 = I2C_CR2_FREQ(36);
    i2c->ccr = I2C_CCR_FS | I2C_CCR_CCR(30);
    i2c->trise = 12;
    i2c->cr1 = I2C_CR1_PE;
    i2c->cr2 |= I2C_CR2_ITERREN;

    /* Initialisation sequence for SSD1306. */
    for (i = 0; i < sizeof(init_cmds); i++) {
        *p++ = 0x80; /* Co=1, Command */
        *p++ = init_cmds[i];
    }

    /* Start off the I2C transaction. */
    p += oled_start_i2c(p);

    /* Send the initialisation command sequence by DMA. */
    i2c->cr2 |= I2C_CR2_DMAEN;
    dma_start(p - (uint8_t *)buffer);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
