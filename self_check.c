#include "self_check.h"
#include "config.h"
#include "key.h"
#include "timer.h"
#include "tm1628.h"
#include "gas_sensor.h"
#include "fan.h"

static unsigned char self_check_stage;
static unsigned char self_check_elapsed_sec;
static unsigned int  self_check_hold_ticks;
static unsigned char self_check_trigger_armed;
static unsigned char self_check_key_seen;
static unsigned char self_check_key_count;
static unsigned char self_check_wait_key_release;
static unsigned char self_check_sensor_ok;
static unsigned int  self_check_sensor_value;

void SelfCheck_Init(void)
{
    self_check_stage = SELF_CHECK_STAGE_IDLE;
    self_check_elapsed_sec = (unsigned char)0;
    self_check_hold_ticks = (unsigned int)0;
    self_check_trigger_armed = (unsigned char)0x01;
    self_check_key_seen = (unsigned char)0;
    self_check_key_count = (unsigned char)0;
    self_check_wait_key_release = (unsigned char)0;
    self_check_sensor_ok = (unsigned char)0;
    self_check_sensor_value = GAS_PM25_INVALID;
}

unsigned char SelfCheck_IsActive(void)
{
    return (self_check_stage != SELF_CHECK_STAGE_IDLE)
        ? (unsigned char)0x01
        : (unsigned char)0x00;
}

unsigned char SelfCheck_Process(
    unsigned char key_now,
    unsigned char key_down,
    unsigned char power_on)
{
    unsigned char sec_tick;
    unsigned char new_keys;

    if (self_check_stage == SELF_CHECK_STAGE_IDLE)
    {
        if (self_check_trigger_armed == (unsigned char)0x00)
        {
            if ((key_now & KEY_MASK_K2) == (unsigned char)0x00)
            {
                self_check_trigger_armed = (unsigned char)0x01;
                self_check_hold_ticks = (unsigned int)0;
            }
        }
        else if ((power_on == (unsigned char)0x00)
                 && ((key_now & KEY_MASK_K2) != (unsigned char)0x00))
        {
            if (self_check_hold_ticks < K2_LONG_PRESS_TICKS)
            {
                self_check_hold_ticks++;
            }

            if (self_check_hold_ticks >= K2_LONG_PRESS_TICKS)
            {
                self_check_trigger_armed = (unsigned char)0x00;
                self_check_stage = SELF_CHECK_STAGE_DISPLAY_ALL_ON;
                self_check_elapsed_sec = (unsigned char)0;
                self_check_key_seen = (unsigned char)0;
                self_check_key_count = (unsigned char)0;
                self_check_wait_key_release = (unsigned char)0x01;
                self_check_sensor_ok = (unsigned char)0x00;
                self_check_sensor_value = GAS_PM25_INVALID;
                self_check_hold_ticks = (unsigned int)0;

                Fan_Stop();
                Gas_PowerOff();
                Timer0_ResetTick();
                TM1628_AllOn();

                return SELF_CHECK_EVENT_ACTIVE;
            }
        }
        else
        {
            self_check_hold_ticks = (unsigned int)0;
        }

        return SELF_CHECK_EVENT_NONE;
    }

    sec_tick = Timer0_PollSecond();

    if (self_check_stage == SELF_CHECK_STAGE_KEY)
    {
        if (self_check_wait_key_release != (unsigned char)0x00)
        {
            if (key_now == (unsigned char)0x00)
            {
                self_check_wait_key_release = (unsigned char)0x00;
                self_check_elapsed_sec = (unsigned char)0;
            }
        }
        else
        {
            new_keys = (unsigned char)(key_down
                & (unsigned char)0x0F
                & (unsigned char)(~self_check_key_seen));

            if (new_keys != (unsigned char)0x00)
            {
                self_check_key_seen =
                    (unsigned char)(self_check_key_seen | new_keys);

                if ((new_keys & KEY_MASK_K1) != (unsigned char)0x00)
                    self_check_key_count++;
                if ((new_keys & KEY_MASK_K2) != (unsigned char)0x00)
                    self_check_key_count++;
                if ((new_keys & KEY_MASK_K3) != (unsigned char)0x00)
                    self_check_key_count++;
                if ((new_keys & KEY_MASK_K4) != (unsigned char)0x00)
                    self_check_key_count++;

                TM1628_SetLeds(self_check_key_seen);
                TM1628_SetPm25Display((unsigned int)self_check_key_count);

                if (self_check_key_count >= (unsigned char)4)
                {
                    self_check_stage = SELF_CHECK_STAGE_FINISH;
                    self_check_elapsed_sec = (unsigned char)0;
                    TM1628_SetLeds((unsigned char)0x0F);
                    TM1628_SetPm25Display((unsigned int)4);
                }
            }
        }
    }

    if (sec_tick != (unsigned char)0x00)
    {
        self_check_elapsed_sec++;

        if ((self_check_stage == SELF_CHECK_STAGE_DISPLAY_ALL_ON)
            && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_DISPLAY_SEGMENTS;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_AllOff();
            TM1628_DigitsAllOn();
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_DISPLAY_SEGMENTS)
                 && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_LEDS_ALL_ON;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_AllOff();
            TM1628_SetLeds((unsigned char)0x0F);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_LEDS_ALL_ON)
                 && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_FAN_LOW;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_SetDefaultDisplay();
            TM1628_SetSpeedDisplay(FAN_SPEED_LEVEL_LOW);
            TM1628_SetLeds(LED_MASK_4);
            Fan_UpdateOutput((unsigned char)0x01, FAN_SPEED_LEVEL_LOW);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_FAN_LOW)
                 && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_FAN_MEDIUM;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_SetSpeedDisplay(FAN_SPEED_LEVEL_MEDIUM);
            Fan_UpdateOutput((unsigned char)0x01, FAN_SPEED_LEVEL_MEDIUM);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_FAN_MEDIUM)
                 && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_FAN_HIGH;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_SetSpeedDisplay(FAN_SPEED_LEVEL_HIGH);
            Fan_UpdateOutput((unsigned char)0x01, FAN_SPEED_LEVEL_HIGH);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_FAN_HIGH)
                 && (self_check_elapsed_sec >= SELF_CHECK_AUTO_STAGE_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_SENSOR_WARMUP;
            self_check_elapsed_sec = (unsigned char)0;
            Fan_Stop();
            Gas_PowerOn();
            Gas_StartWarmup();
            TM1628_SetDefaultDisplay();
            TM1628_SetPm25Display((unsigned int)Gas_GetWarmupRemaining());
            TM1628_SetLeds((unsigned char)0x00);
        }
        else if (self_check_stage == SELF_CHECK_STAGE_SENSOR_WARMUP)
        {
            Gas_TickWarmupSecond();

            if (Gas_IsWarmupDone() != (unsigned char)0x00)
            {
                self_check_sensor_value = Gas_ReadPm25();
                self_check_sensor_ok =
                    (self_check_sensor_value <= GAS_PM25_MAX)
                        ? (unsigned char)0x01
                        : (unsigned char)0x00;
                self_check_stage = SELF_CHECK_STAGE_SENSOR_RESULT;
                self_check_elapsed_sec = (unsigned char)0;

                if (self_check_sensor_ok != (unsigned char)0x00)
                {
                    TM1628_SetPm25Display(self_check_sensor_value);
                    TM1628_SetLeds(LED_MASK_1);
                }
                else
                {
                    TM1628_SetPm25Display((unsigned int)GAS_PM25_INVALID);
                    TM1628_SetLeds(LED_MASK_2);
                }
            }
            else
            {
                TM1628_SetPm25Display(
                    (unsigned int)Gas_GetWarmupRemaining());
            }
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_SENSOR_RESULT)
                 && (self_check_elapsed_sec
                     >= SELF_CHECK_SENSOR_RESULT_SECONDS))
        {
            Gas_PowerOff();
            self_check_stage = SELF_CHECK_STAGE_KEY;
            self_check_elapsed_sec = (unsigned char)0;
            self_check_key_seen = (unsigned char)0;
            self_check_key_count = (unsigned char)0;
            self_check_wait_key_release = (unsigned char)0x01;
            TM1628_SetDefaultDisplay();
            TM1628_SetPm25Display((unsigned int)0);
            TM1628_SetLeds((unsigned char)0x00);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_KEY)
                 && (self_check_elapsed_sec
                     >= SELF_CHECK_KEY_TIMEOUT_SECONDS))
        {
            self_check_stage = SELF_CHECK_STAGE_FINISH;
            self_check_elapsed_sec = (unsigned char)0;
            TM1628_SetPm25Display(
                (unsigned int)self_check_key_count);
            TM1628_SetLeds(self_check_key_seen);
        }
        else if ((self_check_stage == SELF_CHECK_STAGE_FINISH)
                 && (self_check_elapsed_sec >= SELF_CHECK_FINISH_SECONDS))
        {
            Fan_Stop();
            Gas_PowerOff();
            TM1628_AllOff();
            Timer0_ResetTick();

            self_check_stage = SELF_CHECK_STAGE_IDLE;
            self_check_hold_ticks = (unsigned int)0;
            self_check_trigger_armed =
                ((key_now & KEY_MASK_K2) == (unsigned char)0x00)
                    ? (unsigned char)0x01
                    : (unsigned char)0x00;

            return SELF_CHECK_EVENT_FINISHED;
        }
    }

    return SELF_CHECK_EVENT_ACTIVE;
}
