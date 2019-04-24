/*
 * idevicescreenshot.c
 * Gets a screenshot from a device
 *
 * Copyright (C) 2010 Nikias Bassen <nikias@gmx.li>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/screenshotr.h>

static int alarm_pipe[2];
static volatile int stop_running = 0;

static int set_signal_handler(int sig, void (*handler)(int))
{
	struct sigaction sa;
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	return sigaction(sig, &sa, NULL);
}

void sigalarm_handler(int sig)
{
	if (write(alarm_pipe[1], "", 1) != 1) {
		char msg[] = "write: failed from sigalarm_handler\n";
		write(2, msg, sizeof(msg)-1);
		abort();
	}
}

void sigint_handler(int sig)
{
	stop_running = 1;
}

void print_usage(int argc, char **argv);

int main(int argc, char **argv)
{
	idevice_t device = NULL;
	lockdownd_client_t lckd = NULL;
	lockdownd_error_t ldret = LOCKDOWN_E_UNKNOWN_ERROR;
	screenshotr_client_t shotr = NULL;
	lockdownd_service_descriptor_t service = NULL;
	int result = -1;
	int i;
	int rate = 0;
	int append_ext = 0;
	int join = 0;
	const char *udid = NULL;
	char *filename = NULL;

	/* parse cmdline args */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
			idevice_set_debug_level(1);
			continue;
		}
		else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--udid")) {
			i++;
			if (!argv[i] || !*argv[i]) {
				print_usage(argc, argv);
				return 0;
			}
			udid = argv[i];
			continue;
		}
		else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--rate")) {
			i++;
			if (!argv[i] || !*argv[i] || !atoi(argv[i])) {
				print_usage(argc, argv);
				return 0;
			}
			rate = atoi(argv[i]);
			continue;
		}
		if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--ext")) {
			append_ext = 1;
			continue;
		}
		if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--join")) {
			join = 1;
			continue;
		}
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argc, argv);
			return 0;
		}
		else if (argv[i][0] != '-' && !filename) {
			filename = strdup(argv[i]);
			continue;
		}
		else {
			print_usage(argc, argv);
			return 0;
		}
	}

	if (IDEVICE_E_SUCCESS != idevice_new(&device, udid)) {
		if (udid) {
			printf("No device found with udid %s, is it plugged in?\n", udid);
		} else {
			printf("No device found, is it plugged in?\n");
		}
		return -1;
	}

	if (LOCKDOWN_E_SUCCESS != (ldret = lockdownd_client_new_with_handshake(device, &lckd, NULL))) {
		idevice_free(device);
		printf("ERROR: Could not connect to lockdownd, error code %d\n", ldret);
		return -1;
	}

	lockdownd_start_service(lckd, "com.apple.mobile.screenshotr", &service);
	lockdownd_client_free(lckd);
	if (service && service->port > 0) {
		if (screenshotr_client_new(device, service, &shotr) != SCREENSHOTR_E_SUCCESS) {
			printf("Could not connect to screenshotr!\n");
		} else {
			//struct timeval tv;
			char *final_filename = NULL;
			char *imgdata = NULL;
			char c;
			uint64_t imgsize = 0;
			int frame_no = 0;
			FILE *f = NULL;

			if (!filename) {
				time_t now = time(NULL);
				filename = (char*)malloc(36);
				strftime(filename, 36, "screenshot-%Y-%m-%d-%H-%M-%S", gmtime(&now));
				append_ext = 1;
			}

			if (rate) {
				if (pipe(alarm_pipe) != 0) {
					perror("pipe");
					return -1;
				}

				if (set_signal_handler(SIGALRM, sigalarm_handler) != 0) {
					perror("sigaction");
					return -1;
				}

				if (set_signal_handler(SIGINT, sigint_handler) != 0) {
					perror("sigaction");
					return -1;
				}

				long delay = 1000000 / rate;
				struct itimerval it = { { 0, delay }, { 0, delay } };
				if (setitimer(ITIMER_REAL, &it, 0) != 0) {
					perror("setitimer");
					return -1;
				}

				final_filename = (char *)malloc(256);
				memset(final_filename, 0, 256);
			}

			while (!stop_running) {
				// wait for next frame
				if (rate) {
					for (;;) {
						ssize_t alarm_pipe_read_result = read(alarm_pipe[0], &c, 1);
						if ((alarm_pipe_read_result < 0) && (errno == EINTR)) continue;
						//assert(alarm_pipe_read_result == 1);
						break;
					}
				}
				if (screenshotr_take_screenshot(shotr, &imgdata, &imgsize) == SCREENSHOTR_E_SUCCESS) {
					if (append_ext) {
						const char *fileext = NULL;
						if (memcmp(imgdata, "\x89PNG", 4) == 0) {
							fileext = ".png";
						} else if (memcmp(imgdata, "MM\x00*", 4) == 0) {
							fileext = ".tiff";
						} else {
							printf("WARNING: screenshot data has unexpected image format.\n");
							fileext = ".dat";
						}
						strcat(filename, fileext);
						// do it for first (maybe only) image
						append_ext = 0;
					}
					if (rate) {
						snprintf(final_filename, 256, filename, frame_no++);
					} else {
						final_filename = filename;
					}
					if (!join || !f) {
						f = fopen(final_filename, "wb");
					}
					if (f) {
						if (fwrite(imgdata, 1, (size_t)imgsize, f) == (size_t)imgsize) {
							if (!rate) printf("Screenshot saved to %s\n", final_filename);
							result = 0;
						} else {
							printf("Could not save screenshot to file %s!\n", final_filename);
							break;
						}
						if (!join) fclose(f);
					} else {
						printf("Could not open %s for writing: %s\n", final_filename, strerror(errno));
						break;
					}
				} else {
					printf("Could not get screenshot!\n");
					break;
				}
				// finish after first iteration if rate not specified
				if (!rate) break;
			}
			if (join && f) fclose(f);
			screenshotr_client_free(shotr);
		}
	} else {
		printf("Could not start screenshotr service! Remember that you have to mount the Developer disk image on your device if you want to use the screenshotr service.\n");
	}

	if (service)
		lockdownd_service_descriptor_free(service);

	idevice_free(device);
	free(filename);

	return result;
}

void print_usage(int argc, char **argv)
{
	char *name = NULL;

	name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS] [FILE]\n", (name ? name + 1: argv[0]));
	printf("Gets a screenshot from a device.\n");
	printf("The screenshot is saved as a TIFF image with the given FILE name,\n");
	printf("where the default name is \"screenshot-DATE.tiff\", e.g.:\n");
	printf("   ./screenshot-2013-12-31-23-59-59.tiff\n\n");
	printf("NOTE: A mounted developer disk image is required on the device, otherwise\n");
	printf("the screenshotr service is not available.\n\n");
	printf("  -d, --debug\t\tenable communication debugging\n");
	printf("  -u, --udid UDID\ttarget specific device by UDID\n");
	printf("  -r, --rate fps\ttake screenshots at specified frame rate (should by used with --join or filname with %%d printf format specifier)\n");
	printf("  -j, --join\t\tsave screen series joined in single file, suitable for ffmpeg *_pipe inputs\n");
	printf("  -e, --ext\t\tappend extension based on image type to specified filename (default behaviour when not using --rate)\n");
	printf("  -h, --help\t\tprints usage information\n");
	printf("\n");
	printf("Homepage: <" PACKAGE_URL ">\n");
}
