#ifndef __SELF_CHECK_H__
#define __SELF_CHECK_H__

/* Self-check stages */
#define SELF_CHECK_STAGE_IDLE             ((unsigned char)0x00)
#define SELF_CHECK_STAGE_DISPLAY_ALL_ON   ((unsigned char)0x01)
#define SELF_CHECK_STAGE_DISPLAY_SEGMENTS ((unsigned char)0x02)
#define SELF_CHECK_STAGE_LEDS_ALL_ON      ((unsigned char)0x03)
#define SELF_CHECK_STAGE_FAN_LOW          ((unsigned char)0x04)
#define SELF_CHECK_STAGE_FAN_MEDIUM       ((unsigned char)0x05)
#define SELF_CHECK_STAGE_FAN_HIGH         ((unsigned char)0x06)
#define SELF_CHECK_STAGE_KEY              ((unsigned char)0x07)
#define SELF_CHECK_STAGE_FINISH           ((unsigned char)0x08)

#define SELF_CHECK_AUTO_STAGE_SECONDS     ((unsigned char)1)
#define SELF_CHECK_KEY_TIMEOUT_SECONDS    ((unsigned char)10)
#define SELF_CHECK_FINISH_SECONDS         ((unsigned char)2)

/* SelfCheck_Process return values */
#define SELF_CHECK_EVENT_NONE             ((unsigned char)0x00)
#define SELF_CHECK_EVENT_ACTIVE           ((unsigned char)0x01)
#define SELF_CHECK_EVENT_FINISHED         ((unsigned char)0x02)

void SelfCheck_Init(void);
unsigned char SelfCheck_IsActive(void);
unsigned char SelfCheck_Process(
    unsigned char key_now,
    unsigned char key_down,
    unsigned char power_on);

#endif
