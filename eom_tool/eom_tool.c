// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

/*
 * EOM Tool - Eye Opening Monitor (EOM) tool for PCIe device monitoring
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#define _DEFAULT_SOURCE
#include <unistd.h>

#ifndef __KERNEL__
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#include <linux/eom_ioctl.h>

extern char *optarg;

#define EOM_DEVICE "/dev/eom"
#define MAX_SBDFS 8
#define MAX_LANES 16
#define OUTPUT_FILE "./eom_output"
#define MAX_DEVICE_PATH 256
#define MAX_CHIP_INFO_LENGTH 32
#define DONE 1

#define EOM_SLEEP_DELAY_SEC 2
#define THREAD_DEVICE_READY_DELAY_SEC 1
#define SPINNER_UPDATE_MS 200000
#define POLL_TIMEOUT_MS 100
#define DEVICE_CREATION_TIMEOUT_SEC 10
#define DEVICE_CHECK_INTERVAL_US 100000

#define CHIP_INFO_FORMAT_LEN 31
#define FSCANF_CHIP_FORMAT "%31s"
#define FSCANF_CHIP_FAMILY_FORMAT "%31s"

#define DEFAULT_LANE_MASK 0xff
#define DEFAULT_DWELL_TIME_US 100000

typedef enum {
	EOM_SUCCESS = 0,
	EOM_ERROR_INVALID_ARGS = -1,
	EOM_ERROR_DEVICE_OPEN = -2,
	EOM_ERROR_MEMORY_ALLOC = -3,
	EOM_ERROR_IOCTL_FAILED = -4,
	EOM_ERROR_FILE_IO = -5,
	EOM_ERROR_THREAD_FAILED = -6,
	EOM_ERROR_BUFFER_OVERFLOW = -7,
	EOM_ERROR_TIMEOUT = -8
} eom_error_t;

struct eom_target {
	uint16_t segment;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t lane_mask;
	uint8_t lane_count;
	uint8_t skip_select;
	int available_lanes[MAX_LANES];
	int num_available_lanes;
};

struct eom_thread_args {
	int type_index;
	int rc_index;
	int lane;
	volatile int *completed;
	char lane_device[MAX_DEVICE_PATH];
};

struct eom_entry {
	int x;
	int y;
	int error_count;
};

static volatile sig_atomic_t stop_requested = 0;
static pthread_mutex_t stop_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Function prototypes */
static void sigint_handler(int sig);
static eom_error_t validate_parameters(const char *input, struct eom_target *targets,
				       int *num_targets);
static eom_error_t parse_sbdf_list(const char *input, struct eom_target *targets, int *num_targets);
static eom_error_t parse_lane_masks(const char *input, struct eom_target *targets,
				    int *num_targets);
static void print_usage(const char *prog);
static eom_error_t parse_args(int argc, char *argv[], int *type_index, int *dwell_time_us,
			      char *output_file, size_t output_file_size,
			      struct eom_target *targets, int *num_targets, int *all_lanes,
			      int *skip_select, bool *half_eye);
static void *start_eom(void *arg);
static eom_error_t get_chip_info(char *chip_id, int *chip_family, int *platform_version,
				 char *serial_num);
static eom_error_t handle_device_selection(struct eom_target *targets, int num_targets,
					   int type_index, int dwell_time_us,
					   int initial_skip_select);
static eom_error_t run_eom_threads(struct eom_thread_args thread_args[MAX_SBDFS][MAX_LANES],
				   pthread_t threads[MAX_SBDFS][MAX_LANES],
				   volatile int thread_done[MAX_SBDFS][MAX_LANES],
				   struct eom_target *targets, int num_targets, int type_index,
				   int all_lanes);
static eom_error_t write_eom_output(const char *output_file,
				    struct eom_thread_args thread_args[MAX_SBDFS][MAX_LANES],
				    volatile int thread_done[MAX_SBDFS][MAX_LANES],
				    struct eom_target *targets, int num_targets, int all_lanes,
				    int type_index, bool half_eye);
static void cleanup_resources(int lane_fd, int event_fd);
static int is_stop_requested(void);
static void set_stop_requested(int value);

static void sigint_handler(int sig)
{
	(void)sig;
	set_stop_requested(1);
}

/* Thread-safe access to stop_requested flag using mutex */
static int is_stop_requested(void)
{
	int result;
	pthread_mutex_lock(&stop_mutex);
	result = stop_requested;
	pthread_mutex_unlock(&stop_mutex);
	return result;
}

static void set_stop_requested(int value)
{
	pthread_mutex_lock(&stop_mutex);
	stop_requested = value;
	pthread_mutex_unlock(&stop_mutex);
}

static void cleanup_resources(int lane_fd, int event_fd)
{
	if (event_fd >= 0)
		close(event_fd);

	if (lane_fd >= 0)
		close(lane_fd);
}

/* Dynamically discover which lane devices actually exist on the system */
static int discover_available_lanes(int type_index, int segment, int *available_lanes,
				    int max_lanes)
{
	char path[MAX_DEVICE_PATH];
	int count = 0;
	int ret;

	if (!available_lanes || type_index < 0 || type_index >= (int)TYPE_MAX) {
		fprintf(stderr, "Error: Invalid parameters for lane discovery\n");
		return 0;
	}

	fprintf(stdout, "Discovering available lanes for %s segment %d...\n",
		eom_device_names[type_index], segment);

	for (int i = 0; i < max_lanes && count < max_lanes; i++) {
		ret = snprintf(path, sizeof(path), "/dev/eom_%s%d_lane%d",
			       eom_device_names[type_index], segment, i);

		if (ret >= (int)sizeof(path)) {
			fprintf(stderr, "Warning: Device path too long for lane %d\n", i);
			continue;
		}

		if (access(path, F_OK) == 0) {
			available_lanes[count] = i;
			count++;
			fprintf(stdout, "  Found lane device: %s\n", path);
		}
	}

	fprintf(stdout, "Discovered %d available lanes for segment %d\n", count, segment);
	return count;
}

static eom_error_t validate_parameters(const char *input, struct eom_target *targets,
				       int *num_targets)
{
	if (!input || !targets || !num_targets) {
		fprintf(stderr, "Error: Invalid parameters passed to validation function\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	if (*num_targets < 0 || *num_targets > MAX_SBDFS) {
		fprintf(stderr, "Error: Invalid number of targets: %d (max: %d)\n", *num_targets,
			MAX_SBDFS);
		return EOM_ERROR_INVALID_ARGS;
	}

	return EOM_SUCCESS;
}

static eom_error_t parse_sbdf_list(const char *input, struct eom_target *targets, int *num_targets)
{
	char *str, *saveptr1, *token;
	int ret = 0;
	eom_error_t result = EOM_SUCCESS;

	if (validate_parameters(input, targets, num_targets) != EOM_SUCCESS)
		return EOM_ERROR_INVALID_ARGS;

	str = strdup(input);
	if (!str) {
		fprintf(stderr, "Error: Memory allocation failed for SBDF parsing\n");
		return EOM_ERROR_MEMORY_ALLOC;
	}

	token = strtok_r(str, ",", &saveptr1);

	while (token && *num_targets < MAX_SBDFS) {
		/* Parse PCIe SBDF format: supports both S:B:D.F and S:B:D:F formats */
		ret = sscanf(token, "%hu:%hhu:%hhu.%hhu",
					&targets[*num_targets].segment,
					&targets[*num_targets].bus,
					&targets[*num_targets].device,
					&targets[*num_targets].function);

		if (ret != 4) {
			ret = sscanf(token, "%hu:%hhu:%hhu:%hhu",
						&targets[*num_targets].segment,
						&targets[*num_targets].bus,
						&targets[*num_targets].device,
						&targets[*num_targets].function);
		}

		if (ret != 4) {
			fprintf(stderr,
				"Error: Incorrect SBDF Format, expecting S:B:D.F or S:B:D:F: %s\n",
				token);
			result = EOM_ERROR_INVALID_ARGS;
			goto cleanup;
		}

		targets[*num_targets].lane_mask = DEFAULT_LANE_MASK;
		targets[*num_targets].lane_count = MAX_LANES;
		targets[*num_targets].skip_select = 1;
		targets[*num_targets].num_available_lanes = 0;
		memset(targets[*num_targets].available_lanes, 0,
		       sizeof(targets[*num_targets].available_lanes));

		(*num_targets)++;
		token = strtok_r(NULL, ",", &saveptr1);
	}

	if (token && *num_targets >= MAX_SBDFS) {
		fprintf(stderr,
			"Warning: Maximum number of targets (%d) exceeded, ignoring remaining\n",
			MAX_SBDFS);
	}

cleanup:
	free(str);
	return result;
}

static eom_error_t parse_lane_masks(const char *input, struct eom_target *targets, int *num_targets)
{
	char *str, *saveptr2, *token;
	int mask = 0;
	eom_error_t result = EOM_SUCCESS;

	if (validate_parameters(input, targets, num_targets) != EOM_SUCCESS)
		return EOM_ERROR_INVALID_ARGS;

	str = strdup(input);
	if (!str) {
		fprintf(stderr, "Error: Memory allocation failed for lane mask parsing\n");
		return EOM_ERROR_MEMORY_ALLOC;
	}

	token = strtok_r(str, ",", &saveptr2);

	for (int i = 0; i < *num_targets && token; i++) {
		if (sscanf(token, "%hhx", &targets[i].lane_mask) != 1) {
			fprintf(stderr, "Error: Unable to read Lane Mask for target %d: %s\n", i,
				token);
			result = EOM_ERROR_INVALID_ARGS;
			goto cleanup;
		}

		token = strtok_r(NULL, ",", &saveptr2);
		mask = targets[i].lane_mask;

		fprintf(stdout, "Lane mask 0x%x for target %d\n", targets[i].lane_mask, i);

		if (mask == 0) {
			targets[i].lane_count = 0;
			continue;
		}

		/* Count number of set bits in lane mask using builtin function */
		targets[i].lane_count = __builtin_popcount(mask);

		if (targets[i].lane_count > MAX_LANES) {
			fprintf(stderr, "Error: Lane count %d exceeds maximum %d for target %d\n",
				targets[i].lane_count, MAX_LANES, i);
			result = EOM_ERROR_INVALID_ARGS;
			goto cleanup;
		}
	}

cleanup:
	free(str);
	return result;
}

static void print_usage(const char *prog)
{
	if (!prog)
		prog = "eom_tool";

	printf("Usage: %s -d <pcie|usb> -s <sbdf_list> [options]\n", prog);
	printf("Options:\n");
	printf("  -d <pcie|usb>    : Select the type of device (PCIe or USB)\n");
	printf("  -s <sbdf list>   : List of SBDF for PCIe device, comma separated\n");
	printf("                     Format: S:B:D.F or S:B:D:F (e.g., 0:0:0.0,1:0:0.0)\n");
	printf("  -m <lane mask>   : Specify the lane mask (default: 0x%02x)\n", DEFAULT_LANE_MASK);
	printf("  -g               : Force selecting the device\n");
	printf("  -a               : Run EOM on all available lanes\n");
	printf("  -h               : Enable half eye data mode\n");
	printf("  -f <output_file> : Specify the path and output file name\n");
	printf("  -t <dwell_time>  : Specify the dwell time in microseconds (default: %d)\n",
			DEFAULT_DWELL_TIME_US);
	printf("Examples:\n");
	printf("  %s -d pcie -s 0:0:0.0,1:0:0.0 -m 0x3,0x3 -a -f ./eom_output\n", prog);
	printf("  %s -d pcie -s 0:0:0.0,1:0:0.0 -g -a -f ./eom_output -t 200000\n", prog);
	printf("  %s -d usb -s 0:0:0:0 -m 0x1 -h -f ./eom_output\n", prog);
}

static eom_error_t parse_args(int argc, char *argv[], int *type_index, int *dwell_time_us,
			      char *output_file, size_t output_file_size,
			      struct eom_target *targets, int *num_targets, int *all_lanes,
			      int *skip_select, bool *half_eye)
{
	int opt;
	eom_error_t result;

	if (!argv || !type_index || !dwell_time_us || !output_file ||
		!targets || !num_targets || !all_lanes || !skip_select || !half_eye) {
		fprintf(stderr, "Error: Invalid parameters passed to parse_args\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	while ((opt = getopt(argc, argv, "d:s:m:t:f:agh")) != -1) {
		switch (opt) {
		case 'd':
			*type_index = -1;
			for (int i = 0; i < (int)TYPE_MAX; i++) {
				if (strcmp(optarg, eom_device_names[i]) == 0) {
					*type_index = i;
					break;
				}
			}
			if (*type_index == -1) {
				fprintf(stderr, "Error: Invalid device type: %s\n", optarg);
				return EOM_ERROR_INVALID_ARGS;
			}
			break;

		case 't':
			*dwell_time_us = atoi(optarg);
			if (*dwell_time_us <= 0) {
				fprintf(stderr, "Error: Invalid dwell time: %s (must be > 0)\n",
					optarg);
				return EOM_ERROR_INVALID_ARGS;
			}
			break;

		case 's':
			result = parse_sbdf_list(optarg, targets, num_targets);
			if (result != EOM_SUCCESS)
				return result;
			break;

		case 'm':
			result = parse_lane_masks(optarg, targets, num_targets);
			if (result != EOM_SUCCESS)
				return result;
			*all_lanes = 0;
			break;

		case 'a':
			*all_lanes = 1;
			break;

		case 'f':
			if (strlen(optarg) >= output_file_size) {
				fprintf(stderr, "Error: Output file path too long: %s (max: %zu)\n",
					optarg, output_file_size - 1);
				return EOM_ERROR_BUFFER_OVERFLOW;
			}
			if (snprintf(output_file, output_file_size, "%s", optarg) >=
			    (int)output_file_size) {
				fprintf(stderr, "Error: Output file path truncated\n");
				return EOM_ERROR_BUFFER_OVERFLOW;
			}
			printf("Output file: %s\n", output_file);
			break;

		case 'g':
			*skip_select = 0;
			break;

		case 'h':
			*half_eye = true;
			break;

		default:
			print_usage(argv[0]);
			return EOM_ERROR_INVALID_ARGS;
		}
	}

	return EOM_SUCCESS;
}

/* EOM thread function - runs EOM measurement on a single lane */
static void *start_eom(void *arg)
{
	struct eom_thread_args *args = (struct eom_thread_args *)arg;
	int lane_fd = -1;
	int event_fd = -1;
	uint64_t event_value;

	if (!args || !args->completed) {
		fprintf(stderr, "Error: Invalid thread arguments\n");
		return NULL;
	}

	fprintf(stdout, "[Thread] Lane device: %s\n", args->lane_device);

	/* Small delay to ensure device is ready after creation */
	sleep(THREAD_DEVICE_READY_DELAY_SEC);

	lane_fd = open(args->lane_device, O_RDWR);
	if (lane_fd < 0) {
		perror("Failed to open lane device");
		*args->completed = EOM_ERROR_DEVICE_OPEN;
		return NULL;
	}

	fprintf(stdout, "[Thread] Running EOM for %s\n", args->lane_device);

	/* Create eventfd for asynchronous completion notification */
	event_fd = eventfd(0, 0);
	if (event_fd < 0) {
		perror("Failed to create eventfd");
		*args->completed = EOM_ERROR_DEVICE_OPEN;
		cleanup_resources(lane_fd, -1);
		return NULL;
	}

	/* Register eventfd with kernel driver for completion notification */
	if (ioctl(lane_fd, EOM_IOCTL_SET_EVENTFD, &event_fd) < 0) {
		perror("Failed to set eventfd");
		*args->completed = EOM_ERROR_IOCTL_FAILED;
		cleanup_resources(lane_fd, event_fd);
		return NULL;
	}

	/* Start the EOM measurement process */
	if (ioctl(lane_fd, EOM_IOCTL_START_EOM, NULL) < 0) {
		perror("Failed to start EOM");
		*args->completed = EOM_ERROR_IOCTL_FAILED;
		cleanup_resources(lane_fd, event_fd);
		return NULL;
	}

	/* Poll eventfd for completion signal from kernel */
	struct pollfd pfd = {
		.fd = event_fd,
		.events = POLLIN,
	};

	fprintf(stdout, "[Thread] EOM Waiting for completion for lane %d\n", args->lane);

	while (!is_stop_requested()) {
		int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);

		if (ret > 0 && (pfd.revents & POLLIN)) {
			if (read(event_fd, &event_value, sizeof(event_value)) > 0) {
				fprintf(stdout, "[Thread] EOM completed for lane %d\n", args->lane);
				break;
			}
		} else if (ret < 0 && errno != EINTR) {
			perror("Poll failed");
			break;
		}
	}

	if (is_stop_requested()) {
		fprintf(stdout, "Stopping EOM on lane %d due to signal...\n", args->lane);
		if (ioctl(lane_fd, EOM_IOCTL_STOP_EOM, NULL) < 0)
			perror("Failed to stop EOM");
		fflush(stdout);
	}

	cleanup_resources(lane_fd, event_fd);

	*args->completed = DONE;
	fprintf(stdout, "[Thread] Completed EOM for lane %d\n", args->lane);
	fflush(stdout);

	return NULL;
}

static eom_error_t get_chip_info(char *chip_id, int *chip_family, int *platform_version,
				 char *serial_num)
{
	FILE *fp;

	if (!chip_id || !chip_family || !platform_version || !serial_num) {
		fprintf(stderr, "Error: Invalid parameters passed to get_chip_info\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	snprintf(chip_id, MAX_CHIP_INFO_LENGTH, "unknown");
	*chip_family = 0;
	*platform_version = 0;
	snprintf(serial_num, MAX_CHIP_INFO_LENGTH, "unknown");

	fp = fopen("/sys/devices/soc0/chip_id", "r");
	if (fp) {
		if (fscanf(fp, FSCANF_CHIP_FORMAT, chip_id) != 1)
			fprintf(stderr, "Warning: Failed to read chip_id\n");
		fclose(fp);
	} else {
		fprintf(stderr, "Warning: Failed to open /sys/devices/soc0/chip_id\n");
	}

	fp = fopen("/sys/devices/soc0/chip_family", "r");
	if (fp) {
		char chip_family_str[MAX_CHIP_INFO_LENGTH];
		if (fscanf(fp, FSCANF_CHIP_FAMILY_FORMAT, chip_family_str) == 1) {
			*chip_family = (int)strtol(chip_family_str, NULL, 0);
		} else {
			fprintf(stderr, "Warning: Failed to read chip_family\n");
		}
		fclose(fp);
	} else {
		fprintf(stderr, "Warning: Failed to open /sys/devices/soc0/chip_family\n");
	}

	fp = fopen("/sys/devices/soc0/platform_version", "r");
	if (fp) {
		if (fscanf(fp, "%d", platform_version) != 1)
			fprintf(stderr, "Warning: Failed to read platform_version\n");
		fclose(fp);
	} else {
		fprintf(stderr, "Warning: Failed to open /sys/devices/soc0/platform_version\n");
	}

	fp = fopen("/sys/devices/soc0/serial_number", "r");
	if (fp) {
		if (fscanf(fp, FSCANF_CHIP_FORMAT, serial_num) != 1)
			fprintf(stderr, "Warning: Failed to read serial_num\n");
		fclose(fp);
	} else {
		fprintf(stderr, "Warning: Failed to open /sys/devices/soc0/serial_number\n");
	}

	return EOM_SUCCESS;
}

/* Device selection - decides whether to skip device selection based on existing lane devices */
static eom_error_t handle_device_selection(struct eom_target *targets, int num_targets,
					   int type_index, int dwell_time_us,
					   int initial_skip_select)
{
	struct eom_select_device select_dev;
	int eom_fd;
	int ret;

	if (!targets || num_targets <= 0 || type_index < 0 || type_index >= (int)TYPE_MAX) {
		fprintf(stderr, "Error: Invalid parameters for device selection\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	for (int j = 0; j < num_targets; j++)
		targets[j].skip_select = initial_skip_select;

	for (int j = 0; j < num_targets; j++) {
		char path[MAX_DEVICE_PATH];

		/* Auto-detect if device selection is needed by checking if lane devices exist */
		if (initial_skip_select != 0) {
			printf("Target %d: %d:%d:%d.%d lane mask 0x%x lane count %u\n", j,
			       targets[j].segment, targets[j].bus, targets[j].device,
			       targets[j].function, targets[j].lane_mask, targets[j].lane_count);

			for (int i = 0; i < MAX_LANES; i++) {
				ret = snprintf(path, sizeof(path), "/dev/eom_%s%d_lane%d",
					       eom_device_names[type_index], targets[j].segment, i);

				if (ret >= (int)sizeof(path)) {
					fprintf(stderr,
						"Error: Device path too long for target %d lane %d\n",
						j, i);
					return EOM_ERROR_BUFFER_OVERFLOW;
				}

				printf("Checking if dev %s exists?\n", path);
				if (access(path, F_OK) != 0) {
					fprintf(stderr, "  dev %s does not exist, select device\n",
						path);
					targets[j].skip_select = 0;
					break;
				}
			}
		}

		if (targets[j].skip_select == 1) {
			fprintf(stdout,
				"Skip select IOCTL on device, lane devices already exist for %d:%d:%d.%d\n",
				targets[j].segment, targets[j].bus, targets[j].device,
				targets[j].function);
		} else {
			/* Send device selection IOCTL to kernel driver */
			eom_fd = open(EOM_DEVICE, O_RDWR);
			if (eom_fd < 0) {
				perror("Failed to open EOM device");
				return EOM_ERROR_DEVICE_OPEN;
			}

			memset(&select_dev, 0, sizeof(select_dev));
			select_dev.index = targets[j].segment;
			select_dev.type = type_index;
			select_dev.vendor_id = 0;
			select_dev.device_id = 0;
			select_dev.dwell_time_us = dwell_time_us;

			ret = snprintf(select_dev.name, sizeof(select_dev.name), "%s",
				       eom_device_names[type_index]);
			if (ret >= (int)sizeof(select_dev.name)) {
				fprintf(stderr, "Error: Device name too long\n");
				close(eom_fd);
				return EOM_ERROR_BUFFER_OVERFLOW;
			}

			fprintf(stdout, "Selected Device: %s\n", select_dev.name);
			ret = ioctl(eom_fd, EOM_IOCTL_SELECT_DEVICE, &select_dev);
			if (ret < 0) {
				if (errno != EBUSY) {
					perror("Failed to select device");
					fprintf(stderr, "SELECT IOCTL Failed for device for segment %d (ret: %d)\n",
						targets[j].segment, ret);
					close(eom_fd);
					return EOM_ERROR_IOCTL_FAILED;
				}
				fprintf(stdout, "Device already selected, skipping select\n");
			}
			close(eom_fd);

			/* Wait for kernel to create lane device nodes */
			fprintf(stdout, "Waiting for device creation...\n");
			sleep(EOM_SLEEP_DELAY_SEC);
		}

		/* Discover which lane devices actually exist after device selection */
		targets[j].num_available_lanes = discover_available_lanes(
			type_index, targets[j].segment, targets[j].available_lanes, MAX_LANES);

		if (targets[j].num_available_lanes == 0) {
			fprintf(stderr, "Warning: No lane devices found for segment %d\n",
				targets[j].segment);
		}
	}

	return EOM_SUCCESS;
}

/* Thread management - creates threads for each available lane and manages their lifecycle */
static eom_error_t run_eom_threads(struct eom_thread_args thread_args[MAX_SBDFS][MAX_LANES],
				   pthread_t threads[MAX_SBDFS][MAX_LANES],
				   volatile int thread_done[MAX_SBDFS][MAX_LANES],
				   struct eom_target *targets, int num_targets, int type_index,
				   int all_lanes)
{
	int ret;
	int threads_created[MAX_SBDFS][MAX_LANES] = {0};

	if (!thread_args || !threads || !thread_done || !targets ||
		num_targets <= 0 || type_index < 0 || type_index >= (int)TYPE_MAX) {
		fprintf(stderr, "Error: Invalid parameters for running EOM threads\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	/* Create threads only for discovered available lanes */
	for (int j = 0; j < num_targets; j++) {
		fprintf(stdout, "Target %d: %d:%d:%d.%d lane mask 0x%x available lanes %d all_lanes? %d\n",
			j, targets[j].segment, targets[j].bus, targets[j].device, targets[j].function,
			targets[j].lane_mask, targets[j].num_available_lanes, all_lanes);

		for (int lane_idx = 0; lane_idx < targets[j].num_available_lanes; lane_idx++) {
			int lane_num = targets[j].available_lanes[lane_idx];

			/* Skip lanes not in mask unless all_lanes is set */
			if (!all_lanes && !(targets[j].lane_mask & (1 << lane_num))) {
				fprintf(stdout, "Skipping lane %d (not in mask)\n", lane_num);
				continue;
			}

			thread_args[j][lane_num].type_index = type_index;
			thread_args[j][lane_num].rc_index = targets[j].segment;
			thread_args[j][lane_num].lane = lane_num;

			ret = snprintf(thread_args[j][lane_num].lane_device,
				       sizeof(thread_args[j][lane_num].lane_device),
				       "/dev/eom_%s%d_lane%d", eom_device_names[type_index],
				       targets[j].segment, lane_num);

			if (ret >= (int)sizeof(thread_args[j][lane_num].lane_device)) {
				fprintf(stderr,
					"Error: Lane device path too long for target %d lane %d\n",
					j, lane_num);
				return EOM_ERROR_BUFFER_OVERFLOW;
			}

			thread_args[j][lane_num].completed = &thread_done[j][lane_num];

			ret = pthread_create(&threads[j][lane_num], NULL, start_eom,
					     &thread_args[j][lane_num]);
			if (ret != 0) {
				fprintf(stderr, "Error: Failed to create thread for target %d lane %d: %s\n",
					j, lane_num, strerror(ret));
				return EOM_ERROR_THREAD_FAILED;
			}
			threads_created[j][lane_num] = 1;
		}
	}

	/* Progress indicator while waiting for all threads to complete */
	static const char spinner[] = "|/-\\";
	int spinner_index = 0;

	while (!is_stop_requested()) {
		int all_done = 1;

		/* Check if all created threads have completed */
		for (int j = 0; j < num_targets; j++) {
			for (int lane_idx = 0; lane_idx < targets[j].num_available_lanes; lane_idx++) {
				int lane_num = targets[j].available_lanes[lane_idx];

				if (!threads_created[j][lane_num])
					continue;

				if (!all_lanes && !(targets[j].lane_mask & (1 << lane_num)))
					continue;

				if (!thread_done[j][lane_num]) {
					all_done = 0;
					break;
				}
			}
			if (!all_done)
				break;
		}

		if (all_done)
			break;

		if (is_stop_requested())
			break;

		/* Animated spinner to show progress */
		fprintf(stdout, "\rRunning EOM... %c", spinner[spinner_index]);
		fflush(stdout);
		spinner_index = (spinner_index + 1) % 4;
		usleep(SPINNER_UPDATE_MS);
	}

	fprintf(stdout, "\rEOM IOCTLs complete.\n");

	/* Join all created threads to ensure proper cleanup */
	for (int j = 0; j < num_targets; j++) {
		for (int lane_idx = 0; lane_idx < targets[j].num_available_lanes; lane_idx++) {
			int lane_num = targets[j].available_lanes[lane_idx];

			if (!threads_created[j][lane_num])
				continue;

			ret = pthread_join(threads[j][lane_num], NULL);
			if (ret != 0)
				fprintf(stderr, "Error: Failed to join thread [%d][%d]: %s\n",
					j, lane_num, strerror(ret));

			if (thread_done[j][lane_num] < 0)
				fprintf(stderr, "Error in thread [%d][%d], lane device %s\n",
					j, lane_num, thread_args[j][lane_num].lane_device);
		}
	}

	return EOM_SUCCESS;
}

/* Write EOM measurement data to JSON output file */
static eom_error_t write_eom_output(const char *output_file,
				    struct eom_thread_args thread_args[MAX_SBDFS][MAX_LANES],
				    volatile int thread_done[MAX_SBDFS][MAX_LANES],
				    struct eom_target *targets, int num_targets, int all_lanes,
				    int type_index, bool half_eye)
{
	char serial_num[MAX_CHIP_INFO_LENGTH] = { 0 };
	char chip_id[MAX_CHIP_INFO_LENGTH] = { 0 };
	int platform_version = 0;
	int chip_family = 0;
	FILE *fp = NULL;
	eom_error_t result;

	if (!output_file || !thread_args || !thread_done || !targets ||
		num_targets <= 0 || type_index < 0 || type_index >= (int)TYPE_MAX) {
		fprintf(stderr, "Error: Invalid parameters for writing EOM output\n");
		return EOM_ERROR_INVALID_ARGS;
	}

	fp = fopen(output_file, "w");
	if (!fp) {
		perror("Failed to open output file");
		return EOM_ERROR_FILE_IO;
	}

	result = get_chip_info(chip_id, &chip_family, &platform_version, serial_num);
	if (result != EOM_SUCCESS)
		fprintf(stderr, "Warning: Failed to get chip info, using defaults\n");

	/* Write JSON header */
	fprintf(fp, "{\n");
	fprintf(fp, "  \"version\": \"1.0.0\",\n");
	fprintf(fp, "  \"results\": [\n");

	for (int j = 0; j < num_targets; j++) {
		fprintf(fp, "     {\n");
		fprintf(fp, "     \"chip_info\": {\n");
		fprintf(fp, "        \"id\": \"%s\",\n", chip_id);
		fprintf(fp, "        \"family\": %d,\n", chip_family);
		fprintf(fp, "        \"version\": %d,\n", platform_version);
		fprintf(fp, "        \"serial_num\": \"%s\"\n", serial_num);
		fprintf(fp, "     },\n");
		fprintf(fp, "     \"interface\": \"%s\",\n", eom_device_names[type_index]);
		fprintf(fp, "     \"instance\": %d,\n", targets[j].segment);
		fprintf(fp, "     \"time_scale\": 1.95,\n");
		fprintf(fp, "     \"time_units\": \"ps\",\n");
		fprintf(fp, "     \"voltage_scale\": 1.5,\n");
		fprintf(fp, "     \"voltage_units\": \"mV\",\n");
		fprintf(fp, "     \"half_eye_data\": \"%s\",\n", half_eye ? "true" : "false");
		fprintf(fp, "     \"note\": \"\",\n");
		fprintf(fp, "     \"lanes\": [\n");

		int lane_count = 0;
		/* Write data for each lane that was processed */
		for (int lane_idx = 0; lane_idx < targets[j].num_available_lanes; lane_idx++) {
			int lane_num = targets[j].available_lanes[lane_idx];

			if (!all_lanes && !(targets[j].lane_mask & (1 << lane_num)))
				continue;

			if (lane_count > 0)
				fprintf(fp, ",\n");

			fprintf(fp, "        {\n");
			fprintf(fp, "        \"lane_number\": %d,\n", lane_num);
			fprintf(fp, "        \"note\": \"\",\n");
			fprintf(fp, "        \"eye\": [\n");

			/* Read EOM measurement data from lane device if thread completed successfully */
			if (thread_done[j][lane_num] == DONE) {
				int lane_fd = open(thread_args[j][lane_num].lane_device, O_RDWR);

				if (lane_fd < 0) {
					fprintf(stderr, "Warning: Failed to open lane device(%s) for reading\n",
						thread_args[j][lane_num].lane_device);
					fprintf(fp, "              [0, 0, -1]");
				} else {
					struct eom_entry entry;
					int first_entry = 1;
					ssize_t bytes_read;

					/* Read all EOM measurement points from device */
					while ((bytes_read = read(lane_fd, &entry, sizeof(entry))) == sizeof(entry)) {
						if (!first_entry)
							fprintf(fp, ",\n");

						fprintf(fp, "              [%d, %d, %d]", entry.x,
							entry.y, entry.error_count);
						first_entry = 0;
					}

					if (first_entry)
						fprintf(fp, "              [0, 0, 0]");

					close(lane_fd);
				}
			} else {
				fprintf(fp, "              [0, 0, -1]");
				fprintf(stderr, "Warning: Lane %d failed to complete\n", lane_num);
			}

			fprintf(fp, "\n           ]\n");
			fprintf(fp, "        }");
			lane_count++;
		}

		fprintf(fp, "\n        ]\n");
		if (j == num_targets - 1)
			fprintf(fp, "     }\n");
		else
			fprintf(fp, "     },\n");

		fprintf(stdout, "EOM Data completed for RC %d\n", targets[j].segment);
	}

	fprintf(fp, "  ]\n");
	fprintf(fp, "}\n");

	if (fclose(fp) != 0) {
		perror("Failed to close output file");
		return EOM_ERROR_FILE_IO;
	}

	return EOM_SUCCESS;
}

int main(int argc, char *argv[])
{
	volatile int thread_done[MAX_SBDFS][MAX_LANES] = { 0 };
	struct eom_thread_args thread_args[MAX_SBDFS][MAX_LANES];
	char output_file[MAX_DEVICE_PATH] = OUTPUT_FILE;
	pthread_t threads[MAX_SBDFS][MAX_LANES];
	struct eom_target targets[MAX_SBDFS];
	int dwell_time_us = DEFAULT_DWELL_TIME_US;
	int type_index = -1;
	int num_targets = 0;
	int skip_select = 1;
	bool half_eye = false;
	int all_lanes = 1;
	eom_error_t result;

	memset(thread_args, 0, sizeof(thread_args));
	memset(targets, 0, sizeof(targets));

	signal(SIGINT, sigint_handler);

	result = parse_args(argc, argv, &type_index, &dwell_time_us, output_file,
			    sizeof(output_file), targets, &num_targets, &all_lanes, &skip_select, &half_eye);
	if (result != EOM_SUCCESS) {
		fprintf(stderr, "Error: Failed to parse arguments (code: %d)\n", result);
		return EXIT_FAILURE;
	}

	if (type_index == -1) {
		fprintf(stderr, "Error: Device type must be specified with -d option\n");
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (num_targets == 0) {
		fprintf(stderr, "Error: At least one SBDF must be specified with -s option\n");
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	result = handle_device_selection(targets, num_targets, type_index, dwell_time_us,
					 skip_select);
	if (result != EOM_SUCCESS) {
		fprintf(stderr, "Error: Device selection failed (code: %d)\n", result);
		return EXIT_FAILURE;
	}

	result = run_eom_threads(thread_args, threads, thread_done, targets, num_targets,
				 type_index, all_lanes);
	if (result != EOM_SUCCESS) {
		fprintf(stderr, "Error: EOM thread execution failed (code: %d)\n", result);
		return EXIT_FAILURE;
	}

	result = write_eom_output(output_file, thread_args, thread_done, targets, num_targets,
				  all_lanes, type_index, half_eye);
	if (result != EOM_SUCCESS) {
		fprintf(stderr, "Error: Failed to write EOM output (code: %d)\n", result);
		return EXIT_FAILURE;
	}

	printf("EOM tool completed successfully. Output written to: %s\n", output_file);

	return EXIT_SUCCESS;
}
