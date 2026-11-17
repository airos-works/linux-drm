/* killwait: haiku#134 check. Threads blocked in amdgpu_drm's interruptible
   waits (DRM_HAIKU_IOCTL_WAIT_HOTPLUG as app_server's hot plug thread
   does, and read() of DRM events) must let go when a signal comes:
   1. a SIGUSR1 to a thread in WAIT_HOTPLUG (timeout 1000 ms) ends the ioctl
      with EINTR at once;
   2. kill -9 ends a team with a thread in WAIT_HOTPLUG, one in read(), and
      both.
   Prints PASS/FAIL lines, exits 0 when everything passed.
   killwait [device]		default /dev/dri/card0 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#define LX_IOC(dir, type, nr, size) \
	(((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ((type) << 8) | (nr))
struct drm_haiku_wait_hotplug {
	uint32_t generation;
	uint32_t timeout_ms;
};
#define WAIT_HOTPLUG LX_IOC(3, 'd', 0xf3, sizeof(struct drm_haiku_wait_hotplug))

static const char* sDevice = "/dev/dri/card0";
static int sFD = -1;
static volatile int sReady;

static void
on_usr1(int signal)
{
}

/* the hot plug generation now: a wait for it blocks until the next event */
static uint32_t
current_generation(int fd)
{
	struct drm_haiku_wait_hotplug wait;
	memset(&wait, 0, sizeof(wait));
	wait.timeout_ms = 0;
	ioctl(fd, WAIT_HOTPLUG, &wait, sizeof(wait));
	return wait.generation;
}

static void*
hotplug_thread(void* data)
{
	uint32_t generation = current_generation(sFD);
	for (;;) {
		struct drm_haiku_wait_hotplug wait;
		memset(&wait, 0, sizeof(wait));
		wait.generation = generation;
		wait.timeout_ms = 1000;
		sReady = 1;
		ioctl(sFD, WAIT_HOTPLUG, &wait, sizeof(wait));
	}
	return NULL;
}

static void*
read_thread(void* data)
{
	char buffer[256];
	for (;;)
		read(sFD, buffer, sizeof(buffer));
	return NULL;
}

/* 1: a signal ends WAIT_HOTPLUG */
static int
test_signal(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_usr1;
	sigaction(SIGUSR1, &action, NULL);

	pid_t child = fork();
	if (child == 0) {
		sigaction(SIGUSR1, &action, NULL);
		int fd = open(sDevice, O_RDWR);
		if (fd < 0)
			_exit(3);
		struct drm_haiku_wait_hotplug wait;
		memset(&wait, 0, sizeof(wait));
		wait.generation = current_generation(fd);
		wait.timeout_ms = 1000;
		bigtime_t start = system_time();
		int result = ioctl(fd, WAIT_HOTPLUG, &wait, sizeof(wait));
		int error = errno;
		bigtime_t took = system_time() - start;
		printf("  signal: ioctl returned %d (%s) after %lld ms\n", result,
			result != 0 ? strerror(error) : "ok", (long long)took / 1000);
		/* EINTR well before the 1000 ms timeout */
		_exit(result != 0 && error == EINTR && took < 900000 ? 0 : 1);
	}
	usleep(300000);
	kill(child, SIGUSR1);
	for (int i = 0; i < 100; i++) {
		int status;
		if (waitpid(child, &status, WNOHANG) == child) {
			int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			printf("%s: a signal ends WAIT_HOTPLUG (child status %#x)\n",
				ok ? "PASS" : "FAIL", status);
			return ok;
		}
		usleep(100000);
	}
	printf("FAIL: a signal ends WAIT_HOTPLUG: the child is still in the "
		"ioctl 10 s after the signal\n");
	kill(child, SIGKILL);
	return 0;
}

/* 2: kill -9 ends a team with threads in the waits (1 hot plug, 2 read,
   3 both) */
static int
test_kill(int which)
{
	static const char* kWhat[] = { "", "WAIT_HOTPLUG", "read()",
		"WAIT_HOTPLUG and read()" };
	pid_t child = fork();
	if (child == 0) {
		sFD = open(sDevice, O_RDWR);
		if (sFD < 0)
			_exit(3);
		pthread_t a, b;
		if (which & 1)
			pthread_create(&a, NULL, hotplug_thread, NULL);
		if (which & 2)
			pthread_create(&b, NULL, read_thread, NULL);
		for (;;)
			pause();
	}
	usleep(500000);
	bigtime_t start = system_time();
	kill(child, SIGKILL);
	for (int i = 0; i < 100; i++) {
		int status;
		if (waitpid(child, &status, WNOHANG) == child) {
			printf("PASS: kill -9 ends a team waiting in %s (%lld ms, "
				"status %#x)\n", kWhat[which],
				(long long)(system_time() - start) / 1000, status);
			return 1;
		}
		usleep(100000);
	}
	team_info info;
	int32 threads = get_team_info(child, &info) == B_OK ? info.thread_count : -1;
	printf("FAIL: kill -9 ends a team waiting in %s: team %d still there "
		"after 10 s with %d threads\n", kWhat[which], (int)child,
		(int)threads);
	return 0;
}

int
main(int argc, char** argv)
{
	if (argc > 1)
		sDevice = argv[1];
	int fd = open(sDevice, O_RDWR);
	if (fd < 0) {
		printf("FAIL: %s: %s\n", sDevice, strerror(errno));
		return 1;
	}
	close(fd);
	setvbuf(stdout, NULL, _IONBF, 0);
	int ok = test_signal();
	ok &= test_kill(1);
	ok &= test_kill(2);
	ok &= test_kill(3);
	printf("killwait: %s\n", ok ? "all passed" : "FAILED");
	return ok ? 0 : 1;
}
