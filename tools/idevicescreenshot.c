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

#define TOOL_NAME "idevicescreenshot"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#ifndef WIN32
#include <signal.h>
#endif

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/screenshotr.h>

static int alarm_pipe[2];
static volatile int stop_running = 0;

static int set_signal_handler(int sig, void (*handler)(int));
void sigalarm_handler(int sig);
void sigint_handler(int sig);

void get_image_filename(char *imgdata, char **filename);
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
	int join = 0;
	const char *udid = NULL;
	int use_network = 0;
	char *filename = NULL;

#ifndef WIN32
	signal(SIGPIPE, SIG_IGN);
#endif
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
		else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--network")) {
			use_network = 1;
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
		else if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--join")) {
			join = 1;
			continue;
		}
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argc, argv);
			return 0;
		}
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			printf("%s %s\n", TOOL_NAME, PACKAGE_VERSION);
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

	if (IDEVICE_E_SUCCESS != idevice_new_with_options(&device, udid, (use_network) ? IDEVICE_LOOKUP_NETWORK : IDEVICE_LOOKUP_USBMUX)) {
		if (udid) {
			printf("No device found with udid %s.\n", udid);
		} else {
			printf("No device found.\n");
		}
		return -1;
	}

	if (LOCKDOWN_E_SUCCESS != (ldret = lockdownd_client_new_with_handshake(device, &lckd, TOOL_NAME))) {
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
					if (!filename) {
						get_image_filename(imgdata, &filename);
						if (!filename) {
							printf("FATAL: Could not find a unique filename!\n");
							break;
						}
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
					// don't break on screenshot error, just log it
					printf("Could not get screenshot!\n");
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

void get_image_filename(char *imgdata, char **filename)
{
	// If the provided filename already has an extension, use it as is.
	if (*filename) {
		char *last_dot = strrchr(*filename, '.');
		if (last_dot && !strchr(last_dot, '/')) {
			return;
		}
	}

	// Find the appropriate file extension for the filename.
	const char *fileext = NULL;
	if (memcmp(imgdata, "\x89PNG", 4) == 0) {
		fileext = ".png";
	} else if (memcmp(imgdata, "MM\x00*", 4) == 0) {
		fileext = ".tiff";
	} else {
		printf("WARNING: screenshot data has unexpected image format.\n");
		fileext = ".dat";
	}

	// If a filename without an extension is provided, append the extension.
	// Otherwise, generate a filename based on the current time.
	char *basename = NULL;
	if (*filename) {
		basename = (char*)malloc(strlen(*filename) + 1);
		strcpy(basename, *filename);
		free(*filename);
		*filename = NULL;
	} else {
		time_t now = time(NULL);
		basename = (char*)malloc(32);
		strftime(basename, 31, "screenshot-%Y-%m-%d-%H-%M-%S", gmtime(&now));
	}

	// Ensure the filename is unique on disk.
	char *unique_filename = (char*)malloc(strlen(basename) + strlen(fileext) + 7);
	sprintf(unique_filename, "%s%s", basename, fileext);
	int i;
	for (i = 2; i < (1 << 16); i++) {
		if (access(unique_filename, F_OK) == -1) {
			*filename = unique_filename;
			break;
		}
		sprintf(unique_filename, "%s-%d%s", basename, i, fileext);
	}
	if (!*filename) {
		free(unique_filename);
	}
	free(basename);
}

void print_usage(int argc, char **argv)
{
	char *name = NULL;

	name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS] [FILE]\n", (name ? name + 1: argv[0]));
	printf("\n");
	printf("Gets a screenshot from a connected device.\n");
	printf("\n");
	printf("The image is in PNG format for iOS 9+ and otherwise in TIFF format.\n");
	printf("The screenshot is saved as an image with the given FILE name.\n");
	printf("If FILE has no extension, FILE will be a prefix of the saved filename.\n");
	printf("If FILE is not specified, \"screenshot-DATE\", will be used as a prefix\n");
	printf("of the filename, e.g.:\n");
	printf("   ./screenshot-2013-12-31-23-59-59.tiff\n");
	printf("\n");
	printf("NOTE: A mounted developer disk image is required on the device, otherwise\n");
	printf("the screenshotr service is not available.\n");
	printf("\n");
	printf("  -u, --udid UDID\ttarget specific device by UDID\n");
	printf("  -n, --network\t\tconnect to network device\n");
	printf("  -d, --debug\t\tenable communication debugging\n");
	printf("  -r, --rate fps\ttake screenshots at specified frame rate (should by used with --join or filname with %%d printf format specifier)\n");
	printf("  -j, --join\t\tsave screen series joined in single file, suitable for ffmpeg *_pipe inputs\n");
	printf("  -h, --help\t\tprints usage information\n");
	printf("  -v, --version\t\tprints version information\n");
	printf("\n");
	printf("Homepage:    <" PACKAGE_URL ">\n");
	printf("Bug Reports: <" PACKAGE_BUGREPORT ">\n");
}
