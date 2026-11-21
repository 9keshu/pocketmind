/*
 * PocketMind - Ep 5: one-button recorder with haptics
 *
 * Button (D1 = P0.03, to GND):  click = start recording, click again = stop & save
 * Vibration module IN (D3 = P0.29): 1 buzz = started, 2 buzzes = saved
 * Files: /SD:/RECxxx.WAV (16 kHz, 16-bit, mono), flushed every second
 *
 * LEDs:
 *   green on        = ready (idle)
 *   blue on         = recording
 *   red blinks 2x   = SD card problem
 *   red blinks 3x   = could not create file
 *   red blinks 4x   = microphone problem
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <ff.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_RATE     16000
#define BYTES_PER_SAMP  2
#define BLOCK_MS        100
#define BLOCK_SIZE      (SAMPLE_RATE * BYTES_PER_SAMP * BLOCK_MS / 1000)
#define BLOCK_COUNT     16
#define READ_TIMEOUT_MS 1000
#define GAIN            4      /* volume boost; try 3..8 */
#define DISK_NAME       "SD"
#define MOUNT_PT        "/SD:"

#define BTN_PIN         3      /* P0.03 = D1 */
#define MOTOR_PIN       29     /* P0.29 = D3 */
#define CHG_PIN         13     /* P0.13: low = 100 mA charging */
#define DEBOUNCE_MS     300

K_MEM_SLAB_DEFINE_STATIC(mic_slab, BLOCK_SIZE, BLOCK_COUNT, 4);
K_SEM_DEFINE(btn_sem, 0, 1);

static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
static const struct device *dmic = DEVICE_DT_GET(DT_NODELABEL(pdm0));

static struct gpio_callback btn_cb;
static int64_t last_press;

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_PT,
};

struct __packed wav_header {
	char riff[4]; uint32_t riff_size; char wave[4];
	char fmt[4]; uint32_t fmt_size; uint16_t audio_format; uint16_t channels;
	uint32_t sample_rate; uint32_t byte_rate; uint16_t block_align; uint16_t bits_per_sample;
	char data[4]; uint32_t data_size;
};

static void make_header(struct wav_header *h, uint32_t data_bytes)
{
	memcpy(h->riff, "RIFF", 4); h->riff_size = 36 + data_bytes;
	memcpy(h->wave, "WAVE", 4); memcpy(h->fmt, "fmt ", 4);
	h->fmt_size = 16; h->audio_format = 1; h->channels = 1;
	h->sample_rate = SAMPLE_RATE; h->byte_rate = SAMPLE_RATE * BYTES_PER_SAMP;
	h->block_align = BYTES_PER_SAMP; h->bits_per_sample = 16;
	memcpy(h->data, "data", 4); h->data_size = data_bytes;
}

static void fail(int blinks)
{
	gpio_pin_set_dt(&led_blue, 0);
	gpio_pin_set_dt(&led_green, 0);
	while (1) {
		for (int i = 0; i < blinks; i++) {
			gpio_pin_set_dt(&led_red, 1); k_msleep(200);
			gpio_pin_set_dt(&led_red, 0); k_msleep(200);
		}
		k_msleep(1500);
	}
}

static void buzz(int times)
{
	for (int i = 0; i < times; i++) {
		gpio_pin_set(gpio0, MOTOR_PIN, 1); k_msleep(150);
		gpio_pin_set(gpio0, MOTOR_PIN, 0); k_msleep(150);
	}
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	int64_t now = k_uptime_get();

	if (now - last_press > DEBOUNCE_MS) {
		last_press = now;
		k_sem_give(&btn_sem);
	}
}

static void drain_mic(void)
{
	void *buf;
	uint32_t size;

	while (dmic_read(dmic, 0, &buf, &size, 0) == 0) {
		k_mem_slab_free(&mic_slab, buf);
	}
}

static void record_one(void)
{
	char path[32];
	struct fs_dirent ent;
	struct fs_file_t file;
	struct wav_header hdr;
	uint32_t written = 0;

	for (int n = 1; n < 1000; n++) {
		snprintf(path, sizeof(path), MOUNT_PT "/REC%03d.WAV", n);
		if (fs_stat(path, &ent) != 0) {
			break;
		}
	}

	fs_file_t_init(&file);
	if (fs_open(&file, path, FS_O_CREATE | FS_O_WRITE) != 0) {
		fail(3);
	}
	make_header(&hdr, 0);
	if (fs_write(&file, &hdr, sizeof(hdr)) != sizeof(hdr)) {
		fail(3);
	}

	if (dmic_trigger(dmic, DMIC_TRIGGER_START) != 0) {
		fail(4);
	}

	/* skip mic start-up noise (and the buzz) */
	for (int i = 0; i < 3; i++) {
		void *buf; uint32_t size;
		if (dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS) == 0) {
			k_mem_slab_free(&mic_slab, buf);
		}
	}

	gpio_pin_set_dt(&led_blue, 1);
	k_sem_reset(&btn_sem);

	int blocks = 0;

	while (k_sem_take(&btn_sem, K_NO_WAIT) != 0) {
		void *buf;
		uint32_t size;

		if (dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS) != 0) {
			fail(4);
		}

		int16_t *s = (int16_t *)buf;
		for (uint32_t k = 0; k < size / 2; k++) {
			int32_t v = s[k] * GAIN;
			if (v > 32767) v = 32767;
			if (v < -32768) v = -32768;
			s[k] = (int16_t)v;
		}

		if (fs_write(&file, buf, size) == size) {
			written += size;
		}
		k_mem_slab_free(&mic_slab, buf);

		if ((++blocks % 10) == 0) {
			fs_sync(&file);   /* never lose more than 1 s */
		}
	}

	dmic_trigger(dmic, DMIC_TRIGGER_STOP);
	drain_mic();

	make_header(&hdr, written);
	fs_seek(&file, 0, FS_SEEK_SET);
	fs_write(&file, &hdr, sizeof(hdr));
	fs_close(&file);

	gpio_pin_set_dt(&led_blue, 0);
}

int main(void)
{
	gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);

	gpio_pin_configure(gpio1, 10, GPIO_OUTPUT_ACTIVE);       /* mic power */
	gpio_pin_configure(gpio0, CHG_PIN, GPIO_OUTPUT_INACTIVE); /* 100 mA charging */
	gpio_pin_configure(gpio0, MOTOR_PIN, GPIO_OUTPUT_INACTIVE);

	gpio_pin_configure(gpio0, BTN_PIN, GPIO_INPUT | GPIO_PULL_UP);
	gpio_pin_interrupt_configure(gpio0, BTN_PIN, GPIO_INT_EDGE_FALLING);
	gpio_init_callback(&btn_cb, button_pressed, BIT(BTN_PIN));
	gpio_add_callback(gpio0, &btn_cb);

	k_msleep(500);

	if (disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL) != 0 ||
	    fs_mount(&mp) != 0) {
		fail(2);
	}

	if (!device_is_ready(dmic)) {
		fail(4);
	}

	struct pcm_stream_cfg stream = { .pcm_width = 16, .mem_slab = &mic_slab };
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000, .max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40, .max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = { .req_num_streams = 1, .req_num_chan = 1 },
	};
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	cfg.streams[0].pcm_rate = SAMPLE_RATE;
	cfg.streams[0].block_size = BLOCK_SIZE;

	if (dmic_configure(dmic, &cfg) != 0) {
		fail(4);
	}

	buzz(1);  /* hello */

	while (1) {
		gpio_pin_set_dt(&led_green, 1);           /* ready */
		k_sem_take(&btn_sem, K_FOREVER);          /* wait for click */
		gpio_pin_set_dt(&led_green, 0);

		buzz(1);                                   /* started */
		record_one();
		buzz(2);                                   /* saved */
	}
	return 0;
}
