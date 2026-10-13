/*
========================================================================================
BARE-METAL CAN1 TRANSMISSION & RECEPTION FOR TWO-BOARD COMMUNICATION (Without HAL/LL)
++++==================================================================================
 */

#include <stdint.h>

/*
 * ----------------------------------------------------------------------------------
 * 1. CLOCK & PIN CONFIGURATION ANALYSIS & VERIFICATION
 * ----------------------------------------------------------------------------------
 *
 * System Clock Verification:
 * - CPU Core / AHB Clock: 72 MHz (HCLK)
 * - APB1 Clock: 36 MHz (PCLK1). This is the maximum allowed frequency for APB1 on STM32F4.
 *
 * CAN Baud Rate Math:
 * - CAN Clock Source = APB1 Clock = 36 MHz.
 * - Bit Rate = 500 kbit/s = 500,000 bits/sec.
 * - Bit Time = 1 / 500,000 = 2 microseconds.
 * - Time Quanta (t_q) = Prescaler / APB1 Clock = 4 / 36,000,000 = 111.11 ns.
 * - Total Time Quanta per bit = Bit Time / t_q = 2,000 ns / 111.11 ns = 18 t_q.
 * - Allocation of Time Quanta per bit:
 *   * Sync_Seg = 1 t_q (Fixed by CAN protocol standard)
 *   * BS1 (Bit Segment 1) = 13 t_q
 *   * BS2 (Bit Segment 2) = 4 t_q
 *   * Total = Sync_Seg + BS1 + BS2 = 1 + 13 + 4 = 18 t_q.
 *   * Sample Point: (Sync_Seg + BS1) / Total = (1 + 13) / 18 = 77.7%
 *
 * Pin Mapping:
 * - Pin PD0: CAN1_RX (Alternate Function AF9)
 * - Pin PD1: CAN1_TX (Alternate Function AF9)
 * - Pin PA0: Connected to an external push-button (Active-High with pull-down).
 */

/* Base Addresses */
#define RCC_BASE      0x40023800U
#define GPIOA_BASE    0x40020000U
#define GPIOD_BASE    0x40020C00U
#define CAN1_BASE     0x40006400U
#define NVIC_BASE     0xE000E100U

/* RCC (Reset and Clock Control) Registers */
typedef struct {
    volatile uint32_t CR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t CFGR;
    volatile uint32_t CIR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t RESERVED0;
    volatile uint32_t APB1RSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t RESERVED1[2];
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t RESERVED2;
    volatile uint32_t APB1ENR;
    volatile uint32_t APB2ENR;
} RCC_TypeDef;

#define RCC ((RCC_TypeDef *) RCC_BASE)

/* GPIO Registers */
typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

#define GPIOA ((GPIO_TypeDef *) GPIOA_BASE)
#define GPIOD ((GPIO_TypeDef *) GPIOD_BASE)

/* CAN Transmit Mailbox Structure */
typedef struct {
    volatile uint32_t TIR;
    volatile uint32_t TDTR;
    volatile uint32_t TDLR;
    volatile uint32_t TDHR;
} CAN_TxMailBox_TypeDef;

/* CAN Receive FIFO Mailbox Structure */
typedef struct {
    volatile uint32_t RIR;
    volatile uint32_t RDTR;
    volatile uint32_t RDLR;
    volatile uint32_t RDHR;
} CAN_RxFIFO_TypeDef;

/* CAN Filter Bank Structure */
typedef struct {
    volatile uint32_t FR1;
    volatile uint32_t FR2;
} CAN_FilterRegister_TypeDef;

/* CAN Base Registers */
typedef struct {
    volatile uint32_t MCR;
    volatile uint32_t MSR;
    volatile uint32_t TSR;
    volatile uint32_t RF0R;
    volatile uint32_t RF1R;
    volatile uint32_t IER;
    volatile uint32_t ESR;
    volatile uint32_t BTR;
    volatile uint32_t RESERVED0[88];
    CAN_TxMailBox_TypeDef sTxMailBox[3];
    CAN_RxFIFO_TypeDef sFIFOMailBox[2];
    volatile uint32_t RESERVED1[12];
    volatile uint32_t FMR;
    volatile uint32_t FM1R;
    volatile uint32_t RESERVED2;
    volatile uint32_t FS1R;
    volatile uint32_t RESERVED3;
    volatile uint32_t FFA1R;
    volatile uint32_t RESERVED4;
    volatile uint32_t FA1R;
    volatile uint32_t RESERVED5[8];
    CAN_FilterRegister_TypeDef sFilterRegister[28];
} CAN_TypeDef;

#define CAN1 ((CAN_TypeDef *) CAN1_BASE)

/* NVIC Registers */
typedef struct {
    volatile uint32_t ISER[8];
    volatile uint32_t RESERVED0[24];
    volatile uint32_t ICER[8];
    volatile uint32_t RSERVED1[24];
    volatile uint32_t ISPR[8];
    volatile uint32_t RESERVED2[24];
    volatile uint32_t ICPR[8];
    volatile uint32_t RESERVED3[24];
    volatile uint32_t IABR[8];
    volatile uint32_t RESERVED4[56];
    volatile uint8_t  IP[240];
} NVIC_TypeDef;

#define NVIC ((NVIC_TypeDef *) NVIC_BASE)

#define CAN1_RX0_IRQn   20

/* ----------------------------------------------------------------------------------
 * 3. FUNCTION DECLARATIONS & GLOBAL BUFFER
 * ----------------------------------------------------------------------------------
 */
void SystemClock_Config(void);
void CAN1_GPIO_Init(void);
void CAN1_Init(void);
void CAN1_Filter_AcceptAll_Init(void);
uint8_t CAN1_Tx(uint16_t std_id, uint8_t *data, uint8_t len);
void CAN1_RX0_Interrupt_Enable(void);

volatile struct {
    uint16_t id;
    uint8_t length;
    uint8_t payload[8];
    volatile uint8_t flag; /* Becomes 1 when a frame is received */
} rx_frame_buffer = {0};

/* ----------------------------------------------------------------------------------
 * 4. SYSTEM CLOCK SETUP (72 MHz HCLK, 36 MHz APB1)
 * ----------------------------------------------------------------------------------
 */
void SystemClock_Config(void) {
    RCC->CR |= (1U << 16); /* HSEON = 1 */
    while (!(RCC->CR & (1U << 17)));

    *(volatile uint32_t *)(0x40023C00) = 0x102U; /* 2 Wait States for Flash */

    /* PLL Configuration: HSE (8MHz) / 8 * 288 / 4 = 72 MHz SYSCLK */
    RCC->PLLCFGR = 0x00400000U | (1U << 16) | (288U << 6) | 8U;
    RCC->CR |= (1U << 24); /* PLLON = 1 */
    while (!(RCC->CR & (1U << 25)));

    RCC->CFGR = (4U << 10); /* APB1 Prescaler = 2 (36 MHz) */
    RCC->CFGR |= (2U << 0); /* Switch SYSCLK to PLL */
    while (((RCC->CFGR >> 2) & 0x03U) != 2U);
}

/* ----------------------------------------------------------------------------------
 * 5. GPIO CONFIGURATION (PD0=RX, PD1=TX, PA0=BUTTON)
 * ----------------------------------------------------------------------------------
 */
void CAN1_GPIO_Init(void) {
    RCC->AHB1ENR |= (1U << 3) | (1U << 0); /* Enable GPIOD and GPIOA clocks */

    /* Configure PD0 and PD1 as Alternate Function */
    GPIOD->MODER &= ~((3U << 0) | (3U << 2));
    GPIOD->MODER |=  ((2U << 0) | (2U << 2));

    /* Assign Alternate Function 9 (CAN1) */
    GPIOD->AFR[0] &= ~((0xFU << 0) | (0xFU << 4));
    GPIOD->AFR[0] |=  ((9U << 0) | (9U << 4));

    /* High-speed IO */
    GPIOD->OSPEEDR |= ((3U << 0) | (3U << 2));

    /* Pull-up resistors for CAN RX and TX */
    GPIOD->PUPDR &= ~((3U << 0) | (3U << 2));
    GPIOD->PUPDR |=  ((1U << 0) | (1U << 2));

    /* PA0 Digital Input (Button) with Pull-Down */
    GPIOA->MODER &= ~(3U << 0);
    GPIOA->PUPDR &= ~(3U << 0);
    GPIOA->PUPDR |=  (2U << 0);
}

/* ----------------------------------------------------------------------------------
 * 6. CAN CONTROLLER REGISTERS INITIALIZATION
 * ----------------------------------------------------------------------------------
 */
void CAN1_Init(void) {
    RCC->APB1ENR |= (1U << 25); /* Enable CAN1 peripheral clock */

    CAN1->MCR |= (1U << 0);     /* INRQ = 1 (Initialization Request) */
    while (!(CAN1->MSR & (1U << 0)));

    CAN1->MCR &= ~(1U << 1);    /* SLEEP = 0 (Wakeup) */
    while (CAN1->MSR & (1U << 1));

    /* ABOM: Automatic Bus-Off Recovery */
    CAN1->MCR |= (1U << 6);

    /* CAN Baud Rate configuration: Prescaler=4, BS1=13, BS2=4, SJW=1 */
    CAN1->BTR = 0;
    CAN1->BTR |= (3U << 0);     /* BRP = 4 - 1 = 3 */
    CAN1->BTR |= (12U << 16);   /* TS1 = 13 - 1 = 12 */
    CAN1->BTR |= (3U << 20);    /* TS2 = 4 - 1 = 3 */
    CAN1->BTR |= (0U << 24);    /* SJW = 1 - 1 = 0 */

    CAN1->MCR &= ~(1U << 0);    /* Leave Initialization Mode */
    while (CAN1->MSR & (1U << 0));
}

void CAN1_Filter_AcceptAll_Init(void) {
    CAN1->FMR |= (1U << 0);     /* FINIT = 1 */
    CAN1->FA1R &= ~(1U << 0);   /* Deactivate Filter 0 */
    CAN1->FS1R |= (1U << 0);    /* 32-bit single scale */
    CAN1->FM1R &= ~(1U << 0);   /* Mask mode */

    /* Accept all identifiers */
    CAN1->sFilterRegister[0].FR1 = 0;
    CAN1->sFilterRegister[0].FR2 = 0;

    CAN1->FFA1R &= ~(1U << 0);  /* Direct to FIFO 0 */
    CAN1->FA1R |= (1U << 0);    /* Activate Filter 0 */
    CAN1->FMR &= ~(1U << 0);    /* FINIT = 0 */
}

/* ----------------------------------------------------------------------------------
 * 7. TRANSMIT AND RX INTERRUPT ROUTINES
 * ----------------------------------------------------------------------------------
 */
uint8_t CAN1_Tx(uint16_t std_id, uint8_t *data, uint8_t len) {
    uint8_t mbox;
    if (CAN1->TSR & (1U << 26))      mbox = 0;
    else if (CAN1->TSR & (1U << 27)) mbox = 1;
    else if (CAN1->TSR & (1U << 28)) mbox = 2;
    else return 0; /* Mailboxes busy */

    if (len > 8) len = 8;

    CAN1->sTxMailBox[mbox].TIR = (uint32_t)(std_id << 21);
    CAN1->sTxMailBox[mbox].TDTR = len & 0x0FU;

    CAN1->sTxMailBox[mbox].TDLR = ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
                                  ((uint32_t)data[1] << 8)  | ((uint32_t)data[0]);
    CAN1->sTxMailBox[mbox].TDHR = ((uint32_t)data[7] << 24) | ((uint32_t)data[6] << 16) |
                                  ((uint32_t)data[5] << 8)  | ((uint32_t)data[4]);

    CAN1->sTxMailBox[mbox].TIR |= (1U << 0); /* Request transmission */
    return 1;
}

void CAN1_RX0_Interrupt_Enable(void) {
    CAN1->IER |= (1U << 1); /* FMPIE0 = 1 (FIFO 0 message pending interrupt) */
    NVIC->IP[CAN1_RX0_IRQn] = 0x20; /* Priority 2 */
    NVIC->ISER[CAN1_RX0_IRQn >> 5] |= (1U << (CAN1_RX0_IRQn & 0x1FU));
}

void CAN1_RX0_IRQHandler(void) {
    if (CAN1->RF0R & 0x03U) { /* Check pending count */
        rx_frame_buffer.id = (uint16_t)(CAN1->sFIFOMailBox[0].RIR >> 21);
        rx_frame_buffer.length = (uint8_t)(CAN1->sFIFOMailBox[0].RDTR & 0x0FU);

        rx_frame_buffer.payload[0] = (uint8_t)(CAN1->sFIFOMailBox[0].RDLR >> 0);
        rx_frame_buffer.payload[1] = (uint8_t)(CAN1->sFIFOMailBox[0].RDLR >> 8);
        rx_frame_buffer.payload[2] = (uint8_t)(CAN1->sFIFOMailBox[0].RDLR >> 16);
        rx_frame_buffer.payload[3] = (uint8_t)(CAN1->sFIFOMailBox[0].RDLR >> 24);
        rx_frame_buffer.payload[4] = (uint8_t)(CAN1->sFIFOMailBox[0].RDHR >> 0);
        rx_frame_buffer.payload[5] = (uint8_t)(CAN1->sFIFOMailBox[0].RDHR >> 8);
        rx_frame_buffer.payload[6] = (uint8_t)(CAN1->sFIFOMailBox[0].RDHR >> 16);
        rx_frame_buffer.payload[7] = (uint8_t)(CAN1->sFIFOMailBox[0].RDHR >> 24);

        rx_frame_buffer.flag = 1; /* Notify Main Loop */
        CAN1->RF0R |= (1U << 5);  /* Release FIFO 0 mailbox */
    }
}

/* ----------------------------------------------------------------------------------
 * 8. BIDIRECTIONAL APPLICATION LOOP
 * ----------------------------------------------------------------------------------
 */
int main(void) {
    SystemClock_Config();
    CAN1_GPIO_Init();
    CAN1_Init();
    CAN1_Filter_AcceptAll_Init();
    CAN1_RX0_Interrupt_Enable();

    uint8_t tx_data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78};
    uint8_t button_pressed = 0;

    while (1) {
        /* 1. Transmit side (Manual Trigger via Button PA0) */
        if (GPIOA->IDR & (1U << 0)) {
            if (!button_pressed) {
                /* Send standard message payload with ID 0x123 */
                CAN1_Tx(0x123, tx_data, 8);
                tx_data[7]++; /* Mutate data on every press */
                button_pressed = 1;
            }
        } else {
            button_pressed = 0;
        }

        /* 2. Receive side (Interrupt handler auto-response mirror loop) */
        if (rx_frame_buffer.flag) {
            rx_frame_buffer.flag = 0;

            /* Mirror back the message payload with ID incremented by 1 (e.g. 0x124) */
            uint8_t response_payload[8];
            for (int i = 0; i < 8; i++) {
                response_payload[i] = rx_frame_buffer.payload[i];
            }
            CAN1_Tx(rx_frame_buffer.id + 1, response_payload, rx_frame_buffer.length);
        }
    }
}
