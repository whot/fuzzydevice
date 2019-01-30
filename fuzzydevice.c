#include <assert.h>
#include <errno.h>
#include <evemu.h>
#include <limits.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <libudev.h>
#include <libinput.h>
#include <time.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

static FILE *evemu_file;
static FILE *libinput_file;

#define evbit(t_, c_) ((t_) << 16 | (c_))

static int open_restricted(const char *path, int flags,
			   void *data)
{
	int fd;
	fd = open(path, flags);
	return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *data)
{
	close(fd);
}

static const struct libinput_interface simple_interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

static struct libevdev*
init_random_device(const char *name)
{
	struct libevdev *d = libevdev_new();
	long int nbits = (random() % 64) + 1; /* how many bits do we want? */
	struct input_absinfo abs =  {
		.minimum = 0,
		.maximum = 100,
	};

	libevdev_set_name(d, name);

	while (nbits) {
		int type, code, max;

		type = (random() % EV_MAX) + 1; /* ignore EV_SYN */
		if (type == EV_REP)
			continue;

		max = libevdev_event_type_get_max(type);
		if (max == -1)
			continue;

		code = random() % (max + 1);

		libevdev_enable_event_code(d, type, code,
					   (type == EV_ABS) ? &abs : NULL);

		nbits--;
	}

	return d;
}

static void
drain_events(struct libinput *li)
{
	struct libinput_event *event;

	libinput_dispatch(li);
	while ((event = libinput_get_event(li))) {
		libinput_event_destroy(event);
		libinput_dispatch(li);
	}
}

static void
send_events(struct libevdev *d,
	    struct libevdev_uinput *uinput,
	    struct libinput *li)
{
	size_t nframes = random() % 200; /* how many event frames to send */
	struct bits {
		unsigned int type;
		unsigned int code;
	} bits[EV_MAX * KEY_MAX];
	size_t nbits = 0;
	unsigned int type, code;
	struct timespec tp_last;

	for (type = 1; type <= EV_MAX; type++) {
		int max;

		if (!libevdev_has_event_type(d, type))
			continue;

		max = libevdev_event_type_get_max(type);
		if (max == -1)
			continue;

		for (code = 0; code <= (unsigned int)max; code++) {
			if (!libevdev_has_event_code(d, type, code))
				continue;

			/* Blacklist a few codes so we don't shut the
			 * machine down halfway */
			switch (evbit(type, code)){
			case evbit(EV_SW, SW_RFKILL_ALL):
			case evbit(EV_SW, SW_TABLET_MODE):
			case evbit(EV_SW, SW_LID):
			case evbit(EV_KEY, KEY_POWER):
			case evbit(EV_KEY, KEY_POWER2):
			case evbit(EV_KEY, KEY_SLEEP):
			case evbit(EV_KEY, KEY_SUSPEND):
			case evbit(EV_KEY, KEY_RESTART):
				continue;
			default:
				break;
			}

			bits[nbits].type = type;
			bits[nbits].code = code;
			nbits++;
		}
	}

	if (nbits == 0)
		return;

	clock_gettime(CLOCK_MONOTONIC, &tp_last);

	while (nframes--) {
		size_t nevents = random() % 12; /* events per frame */
		struct timespec tp;
		unsigned long dt = 0;

		clock_gettime(CLOCK_MONOTONIC, &tp);

		dt = (tp.tv_sec * 1E6 + tp.tv_nsec/1000) - (tp_last.tv_sec * 1E6 +
							    tp_last.tv_nsec/1000);

		while (nevents--) {
			int value;
			int idx;

			idx = random() % nbits;
			type = bits[idx].type;
			code = bits[idx].code;

			switch(type) {
			case EV_KEY:
			case EV_SW:
				value = random() % 2;
				break;
			default:
				value = random() % 50;
				break;
			}


			/* evemu format */
			fprintf(evemu_file,
				"E: %lu.%06lu %04x %04x %04d    ",
				tp.tv_sec,
				tp.tv_nsec / 1000,
				type, code, value);
			fprintf(evemu_file,
				"# %s / %-20s %d\n",
				libevdev_event_type_get_name(type),
				libevdev_event_code_get_name(type, code),
				value);
			libevdev_uinput_write_event(uinput, type, code, value);
		}

		fprintf(evemu_file,
			"E: %lu.%06lu %04x %04x %04d    ",
			tp.tv_sec, tp.tv_nsec / 1000, EV_SYN, SYN_REPORT, 0);
		fprintf(evemu_file,
			"# ------------ %s (%d) ---------- %+ldms\n",
			libevdev_event_code_get_name(EV_SYN, SYN_REPORT),
			0,
			dt);

		libevdev_uinput_write_event(uinput, EV_SYN, SYN_REPORT, 0);
		drain_events(li);
	}
}

__attribute__((format (printf, 3, 0)))
static void
log_handler(struct libinput *libinput,
	    enum libinput_log_priority pri,
	    const char *format,
	    va_list args)
{
	vfprintf(libinput_file, format, args);
}

static struct
udev_device *poll_for_udev_event(struct udev_monitor *monitor, int ms)
{
	struct pollfd fds = {0};

	fds.fd = udev_monitor_get_fd(monitor);
	fds.events = POLLIN;

	if (!poll(&fds, 1, ms))
		return NULL;

	return udev_monitor_receive_device(monitor);
}

static void
test_one_device(struct udev *udev, struct udev_monitor *monitor,
		const char *name, unsigned int seed, long int rand)
{
	struct libinput *li;
	struct libevdev_uinput *uinput = NULL;
	struct libevdev *d;
	char evemu_name[64], libinput_name[64];
	int rc;
	struct evemu_device *device;
	struct udev_device *udev_device;
	int fd;
	bool found = false;

	snprintf(evemu_name, sizeof(evemu_name), "%s.evemu", name);
	evemu_file = fopen(evemu_name, "w");
	setbuf(evemu_file, NULL);

	snprintf(libinput_name, sizeof(libinput_name), "%s.libinput", name);
	libinput_file = fopen(libinput_name, "w");
	setbuf(libinput_file, NULL);

	d = init_random_device(name);
	assert(d);
	rc = libevdev_uinput_create_from_device(d,
						LIBEVDEV_UINPUT_OPEN_MANAGED,
						&uinput);
	assert(rc == 0);

	while (!found) {
		udev_device = poll_for_udev_event(monitor, 2000);
		if (!udev_device)
			usleep(200000);
		else if (!strcmp("add", udev_device_get_action(udev_device)) &&
			 udev_device_get_devnode(udev_device) &&
			 !strcmp(udev_device_get_devnode(udev_device), libevdev_uinput_get_devnode(uinput)))
			found = true;
		udev_device_unref(udev_device);
	}

	device = evemu_new(NULL);
	setbuf(stdout, NULL);
	fd = open(libevdev_uinput_get_devnode(uinput), O_RDWR);
	evemu_extract(device, fd);
	evemu_write(device, evemu_file);
	close(fd);

	fprintf(evemu_file, "#\n# seed: %d\n# random: %ld\n#\n", seed, rand);

	li = libinput_udev_create_context(&simple_interface, NULL, udev);
	assert(li);
	libinput_log_set_handler(li, log_handler);
	libinput_log_set_priority(li, LIBINPUT_LOG_PRIORITY_DEBUG);
	libinput_udev_assign_seat(li, "seat0");
	drain_events(li);

	send_events(d, uinput, li);

	libevdev_uinput_destroy(uinput);
	libevdev_free(d);
	libinput_unref(li);
	evemu_delete(device);

	while (udev_device) {
		udev_device = udev_monitor_receive_device(monitor);
		udev_device_unref(udev_device);
	}

	fclose(evemu_file);
	fclose(libinput_file);

	unlink(evemu_name);
	unlink(libinput_name);

	evemu_file = stdout;
	libinput_file = stderr;
}

static bool stop = false;

static void
sighandler(int sig)
{
	stop = true;
}

static void usage(void)
{
	printf("Usage: %s [--help] [--seed=123] [--limit=10] [--random=123456]\n"
	       "\n"
	       "Options:\n"
	       "--seed ..... set the random number generator seed\n"
	       "--limit .... stop after limit iterations\n"
	       "--random ... reproduce only for the device with that random number\n"
	       "",
	       program_invocation_short_name);
}

int
main (int argc, char **argv)
{
	enum options {
		OPT_HELP = 1,
		OPT_SEED,
		OPT_LIMIT,
		OPT_RANDOM,
	};
	struct option opts[] = {
		{ "seed", required_argument, 0, OPT_SEED },
		{ "limit", required_argument, 0, OPT_LIMIT },
		{ "random", required_argument, 0, OPT_RANDOM },
		{ 0, 0, 0, 0 },
	};
	struct udev *udev;
	struct udev_monitor *monitor;
	unsigned int iteration = 0;
	unsigned int limit = INT_MAX;
	int ret;
	long int random_number = 0;
	unsigned int seed = (unsigned int)time(NULL);

	if (getuid() != 0) {
		fprintf(stderr, "Run me as root\n");
		return 77;
	}

	while (1) {
		int c;
		int option_index = 0;

		c = getopt_long(argc, argv, "h", opts, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
		case OPT_HELP:
			usage();
			return 0;
		case OPT_SEED:
			seed = atoi(optarg);
			break;
		case OPT_LIMIT:
			limit = atoi(optarg);
			break;
		case OPT_RANDOM:
			limit = 1;
			random_number = atoi(optarg);
			break;
		}
	}

	srandom(seed);

	setbuf(stdout, NULL);

	evemu_file = stdout;
	libinput_file = stderr;

	udev = udev_new();
	assert(udev);

	monitor = udev_monitor_new_from_netlink(udev, "udev");
	assert(monitor);

	ret = udev_monitor_filter_add_match_subsystem_devtype(monitor,
							      "input",
							      NULL);
	assert(ret == 0);

	ret = udev_monitor_filter_update(monitor);
	assert(ret == 0);

	ret = udev_monitor_enable_receiving(monitor);
	assert(ret == 0);

	signal(SIGINT, sighandler);

	long int r = random();
	if (random_number) {
		printf("Advancing to random number %ld\n", random_number);

		while (r != random_number)
			r = random();
	}

	while (!stop && iteration < limit) {
		char name[64];

		if (random_number && r != random_number) {
			printf(".");
			continue;
		}

		snprintf(name, sizeof(name), "fuzzydevice-%06d", iteration);

		printf("\rTesting %s (seed %10d random %10ld)", name, seed, r);
		test_one_device(udev, monitor, name, seed, r);
		iteration++;
		r = random();
	}

	udev_monitor_unref(monitor);
	udev = udev_unref(udev);
	assert(udev == NULL);

	printf("\n");

	return 0;
}

