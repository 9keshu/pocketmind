/*
 * PocketMind - first recording test
 * Records RECORD_SECONDS of audio from the XIAO Sense PDM mic
 * and saves it as /SD:/RECxxx.WAV (16 kHz, 16-bit, mono).
 *
 * LEDs:
 *   blue  on        = recording
 *   green on        = saved OK (press reset to record again)
 *   red blinks 2x   = SD card problem (wiring / card)
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
#define BLOCK_SIZE      (SAMPLE_RATE * BYTES_PER_SAMP * BLOCK_MS / 1000) /* 3200 B */
#define BLOCK_COUNT     16
#define RECORD_SECONDS  10
#define READ_TIMEOUT_MS 1000
#define DISK_NAME       "SD"
#define MOUNT_PT        "/SD:"

K_MEM_SLAB_DEFINE_STATIC(mic_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_PT,
};

struct __packed wav_header {
	char     riff[4];
	uint32_t riff_size;
	char     wave[4];
	char     fmt[4];
	uint32_t fmt_size;
	uint16_t audio_format;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	char     data[4];
	uint32_t data_size;
};

static void make_header(struct wav_header *h, uint32_t data_bytes)
{
	memcpy(h->riff, "RIFF", 4);
	h->riff_size = 36 + data_bytes;
	memcpy(h->wave, "WAVE", 4);
	memcpy(h->fmt, "fmt ", 4);
	h->fmt_size = 16;
	h->audio_format = 1; /* PCM */
	h->channels = 1;
	h->sample_rate = SAMPLE_RATE;
	h->byte_rate = SAMPLE_RATE * BYTES_PER_SAMP;
	h->block_align = BYTES_PER_SAMP;
	h->bits_per_sample = 16;
	memcpy(h->data, "data", 4);
	h->data_size = data_bytes;
}

static void fail(int blinks)
{
	gpio_pin_set_dt(&led_blue, 0);
	gpio_pin_set_dt(&led_green, 0);
	while (1) {
		for (int i = 0; i < blinks; i++) {
			gpio_pin_set_dt(&led_red, 1);
			k_msleep(200);
			gpio_pin_set_dt(&led_red, 0);
			k_msleep(200);
		}
		k_msleep(1500);
	}
}

int main(void)
{
	char path[32];
	struct fs_file_t file;
	struct fs_dirent ent;
	struct wav_header hdr;
	uint32_t written = 0;
	int ret;

	gpio_pin_configure_dt(&led_red, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_blue, GPIO_OUTPUT_INACTIVE);

	/* Power the onboard mic (MIC_PWR = P1.10 on XIAO Sense) */
	const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));
	gpio_pin_configure(gpio1, 10, GPIO_OUTPUT_ACTIVE);

	k_msleep(500);

	/* 1. SD card */
	if (disk_access_ioctl(DISK_NAME, DISK_IOCTL_CTRL_INIT, NULL) != 0) {
		fail(2);
	}
	if (fs_mount(&mp) != 0) {
		fail(2);
	}

	/* 2. Next free file name: REC001.WAV, REC002.WAV, ... */
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

	/* 3. Microphone */
	const struct device *dmic = DEVICE_DT_GET(DT_NODELABEL(pdm0));

	if (!device_is_ready(dmic)) {
		fail(4);
	}

	struct pcm_stream_cfg stream = {
		.pcm_width = 16,
		.mem_slab = &mic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
		},
	};
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	cfg.streams[0].pcm_rate = SAMPLE_RATE;
	cfg.streams[0].block_size = BLOCK_SIZE;

	if (dmic_configure(dmic, &cfg) != 0) {
		fail(4);
	}
	if (dmic_trigger(dmic, DMIC_TRIGGER_START) != 0) {
		fail(4);
	}

	/* Throw away the first 0.5 s (mic start-up noise) */
	for (int i = 0; i < 5; i++) {
		void *buf;
		uint32_t size;

		if (dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS) == 0) {
			k_mem_slab_free(&mic_slab, buf);
		}
	}

	/* 4. Record */
	gpio_pin_set_dt(&led_blue, 1);

	int blocks = RECORD_SECONDS * 1000 / BLOCK_MS;

	for (int i = 0; i < blocks; i++) {
		void *buf;
		uint32_t size;

		ret = dmic_read(dmic, 0, &buf, &size, READ_TIMEOUT_MS);
		if (ret != 0) {
			fail(4);
		}
		if (fs_write(&file, buf, size) == size) {
			written += size;
		}
		k_mem_slab_free(&mic_slab, buf);

		/* Never lose audio: flush to the card every second */
		if ((i % 10) == 9) {
			fs_sync(&file);
		}
	}

	dmic_trigger(dmic, DMIC_TRIGGER_STOP);

	/* 5. Fix up the WAV header with the real length and close */
	make_header(&hdr, written);
	fs_seek(&file, 0, FS_SEEK_SET);
	fs_write(&file, &hdr, sizeof(hdr));
	fs_close(&file);
	fs_unmount(&mp);

	gpio_pin_set_dt(&led_blue, 0);
	gpio_pin_set_dt(&led_green, 1);

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
