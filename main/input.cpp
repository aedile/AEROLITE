/*
 * input.cpp - medal controls for Asteroids (held upright, like Pac-Man)
 *
 * Asteroids wants five controls off one button and a tilt sensor:
 *   tilt left / right   -> rotate, the direction of gravity within the panel plane
 *   tilt away from you  -> thrust, how far gravity leaves that plane
 *   BOOT button tap     -> fire
 *   BOOT button held    -> hyperspace, on the way down rather than the release, so it feels
 *                          like the panic button it is
 *   PWR short press     -> coin, then start half a second later; long press (1 s) -> power off
 *
 * Both axes are measured against a neutral pose captured on the first IMU read and again on
 * each coin. Held upright gravity lies in the plane of the panel, so the raw pitch is pinned
 * near its limit and cannot swing both ways - measuring against "however I am holding it right
 * now" is what makes the second axis usable at all.
 */
#include "input.h"
#include "qmi8658.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include "audio_hal.h"

static const char *TAG = "INPUT";
#define PIN_BTN_BOOT GPIO_NUM_9
#define PIN_BTN_PWR  GPIO_NUM_18
#define PIN_BAT_EN   GPIO_NUM_15
#define IMU_PERIOD_US 16000

#define ROT_DEADBAND_DEG    5.0f    /* no rotation inside this much tilt */
#define THRUST_DEG         12.0f    /* tip this far away from you to thrust */
#define ROT_SIGN   (+1.0f)          /* flip if left/right are reversed */
#define THRUST_SIGN (+1.0f)         /* flip if thrust triggers the wrong way */
#define HOLD_HYPER_US 700000        /* hold the button this long for hyperspace */

static bool imu_ok, pwr_was_down, fire_was_down, hyper_held;
static int64_t pwr_down_since, imu_last_us, coin_seq_start, boot_down_since;
static int coin_seq;                     /* 0 idle, 1 coin held, 2 gap, 3 start held */
static float neutral_lr, neutral_ud;
static bool have_neutral;
static int64_t last_log;
static float dbg_roll, dbg_pitch;
static int8_t rot_dir;                   /* -1 left, +1 right, 0 centred */
static bool thrusting;

/* angles of gravity, in degrees: lr = within the panel plane, ud = out of it */
/*
 * One reading, and whether it can be trusted. These angles are the direction of gravity within
 * the panel's plane and how far out of that plane it lies, and they only mean anything while
 * the medal is being held up: lying flat on a desk, gravity points straight out of the screen
 * and the in-plane angle is noise. Zeroing on a reading like that - which is exactly what
 * happened, because the first one was taken at boot with the medal on a desk - sets a centre
 * you were never holding, and leaves a control that works in one direction and not the other.
 */
static bool read_angles(float *lr, float *ud)
{
    int16_t ax, ay, az;
    qmi8658_read_accel(&ax, &ay, &az);
    float in_plane = sqrtf((float)ax * ax + (float)ay * ay);
    *lr = atan2f((float)ay, (float)ax) * 57.2958f;
    *ud = atan2f((float)az, in_plane) * 57.2958f;
    return in_plane > 1.2f * fabsf((float)az);      /* held up, not lying down */
}

static bool capture_neutral(void)
{
    float lr, ud;
    if (!imu_ok || !read_angles(&lr, &ud)) return false;   /* try again next time */
    neutral_lr = lr; neutral_ud = ud;
    have_neutral = true;
    rot_dir = 0; thrusting = false;
    return true;
}

static inline float wrap_deg(float d)
{
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

void input_init(void)
{
    gpio_config_t bat = {}; bat.pin_bit_mask = 1ULL << PIN_BAT_EN; bat.mode = GPIO_MODE_OUTPUT; gpio_config(&bat);
    gpio_set_level(PIN_BAT_EN, 1);
    gpio_config_t io = {}; io.pin_bit_mask = (1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_PWR); io.mode = GPIO_MODE_INPUT; io.pull_up_en = GPIO_PULLUP_ENABLE; gpio_config(&io);
    i2c_config_t i2c = {}; i2c.mode = I2C_MODE_MASTER; i2c.sda_io_num = GPIO_NUM_8; i2c.scl_io_num = GPIO_NUM_7;
    i2c.sda_pullup_en = GPIO_PULLUP_ENABLE; i2c.scl_pullup_en = GPIO_PULLUP_ENABLE; i2c.master.clk_speed = 100000;
    i2c_param_config(I2C_NUM_0, &i2c);
    esp_err_t err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "I2C init failed: %s", esp_err_to_name(err));
    imu_ok = qmi8658_init();
    ESP_LOGI(TAG, "input ready (IMU %s); neutral pose is captured on the first read and on each coin", imu_ok ? "ok" : "missing");
}


/* Holding the button for three seconds toggles the sound off and on. This is a thing people
 * wear places, and some of those places need to be quiet. */
#define HOLD_MUTE_US 3000000
static int64_t mute_down_since;
static bool mute_armed, mute_fired;

static void mute_gesture(bool boot, int64_t now)
{
    if (boot && !mute_armed) { mute_armed = true; mute_fired = false; mute_down_since = now; }
    if (!boot) { mute_armed = false; return; }
    if (!mute_fired && now - mute_down_since >= HOLD_MUTE_US) {
        mute_fired = true;
        audio_set_mute(!audio_get_mute());
        ESP_LOGI("INPUT", "sound %s", audio_get_mute() ? "off" : "on");
    }
}

void input_update(ast_input_t *in)
{
    int64_t now = esp_timer_get_time();
    bool boot = gpio_get_level(PIN_BTN_BOOT) == 0;
    mute_gesture(boot, now);
    bool pwr = gpio_get_level(PIN_BTN_PWR) == 0;

    /* The button is passed through as a level, exactly as the cabinet wires it: the game does
     * its own edge detection and fires once per press. A one-pass pulse here would be wrong -
     * this function runs on every loop iteration, including the ones that emulate no frame at
     * all, so a pulse can be raised and cleared without the CPU ever seeing it. That is what
     * made shooting feel unreliable. Holding past the threshold is hyperspace, and drops fire
     * so the two do not fight; the shot has already gone out by then. */
    if (boot && !fire_was_down) {
        if (!have_neutral) capture_neutral();
        boot_down_since = now; hyper_held = false;
    }
    if (boot && now - boot_down_since >= HOLD_HYPER_US) hyper_held = true;
    if (!boot) hyper_held = false;
    fire_was_down = boot;
    in->fire = (boot && !hyper_held) ? 1 : 0;
    in->hyperspace = hyper_held ? 1 : 0;

    if (pwr && !pwr_was_down) pwr_down_since = now;
    if (pwr && now - pwr_down_since >= 1000000) {
        ESP_LOGI(TAG, "power off");
        gpio_set_level(PIN_BAT_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!pwr && pwr_was_down && now - pwr_down_since < 400000 && coin_seq == 0) {
        coin_seq = 1; coin_seq_start = now;
        capture_neutral();                    /* a coin also re-centres however you are holding it */
    }
    pwr_was_down = pwr;

    /* coin/start sequence: coin 100 ms, gap 400 ms, start 100 ms */
    int64_t el = now - coin_seq_start;
    in->coin1 = 0; in->start1 = 0;
    switch (coin_seq) {
        case 1: in->coin1 = 1; if (el > 100000) coin_seq = 2; break;
        case 2: if (el > 500000) { coin_seq = 3; capture_neutral(); }  /* settled into playing posture */
            break;
        case 3: in->start1 = 1; if (el > 600000) coin_seq = 0; break;
        default: break;
    }

    float lr, ud;
    bool imu_due = imu_ok && now - imu_last_us >= IMU_PERIOD_US;
    if (imu_due) imu_last_us = now;
    /* nothing is captured or acted on until the medal is actually being held up */
    if (imu_due && read_angles(&lr, &ud) && (have_neutral || capture_neutral())) {
        float roll = wrap_deg(lr - neutral_lr) * ROT_SIGN;
        float pitch = wrap_deg(ud - neutral_ud) * THRUST_SIGN;
        dbg_roll = roll; dbg_pitch = pitch;
        rot_dir = (roll > ROT_DEADBAND_DEG) ? +1 : (roll < -ROT_DEADBAND_DEG) ? -1 : 0;
        thrusting = pitch > THRUST_DEG;
    }
    in->left  = (rot_dir < 0) ? 1 : 0;
    in->right = (rot_dir > 0) ? 1 : 0;
    in->thrust = thrusting ? 1 : 0;

    if (now - last_log >= 2000000) {         /* so the controls can be checked over serial */
        last_log = now;
        ESP_LOGI(TAG, "imu %d neutral %d  roll %+6.1f pitch %+6.1f -> L%d R%d T%d  fire %d hyper %d",
                 imu_ok, have_neutral, (double)dbg_roll, (double)dbg_pitch,
                 in->left, in->right, in->thrust, in->fire, in->hyperspace);
    }
}
