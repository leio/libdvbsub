#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h> /* uint16_t, ... */
#include <stdio.h> /* perror, ... */
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>
#include <stdlib.h> /* exit */

static int s_demux_file = -1;

static int32_t setupDVBSubPid(const char* demux_device, uint16_t pid)
{
	if (pid != 0) {
		s_demux_file = open(demux_device, O_RDWR | O_NONBLOCK);
		if (s_demux_file < 0) {
			perror("open demux file");
			return s_demux_file;
		}
	} else {
		return -1;
	}

	struct dmx_pes_filter_params pesfilter = {
		.pid=pid,
		.input = DMX_IN_DVR,
		.output = DMX_OUT_TAP,
		.pes_type = DMX_PES_SUBTITLE,
		.flags = 0
	};

	int ret = ioctl(s_demux_file, DMX_SET_PES_FILTER, &pesfilter);
	if (ret != 0) {
		perror("ioctl DMX_SET_PES_FILTER");
		return ret;
	}

	ret = ioctl(s_demux_file, DMX_START);
	if (ret != 0) {
		perror("ioctl DMX_START");
		return ret;
	} else {
		printf("Demuxing started on pid %u\n", pid);
	}

	return 0;
}

static int32_t closeDVBSubPid()
{
	if (s_demux_file >= 0)
		close(s_demux_file);
}

#define MAX_READ 4096
int main(void)
{
	fd_set rfds;
	struct timeval tv;
	int select_ret;
	char buf[2*MAX_READ]; // FIXME: Too large, but menat just as a test
	ssize_t read_len, write_len;

	FD_ZERO(&rfds);

	if (setupDVBSubPid("/dev/dvb/adapter0/demux0", 1027) < 0) {
		fprintf(stderr, "PID setup failed, bailing out!\n");
		exit(2);
	}

	FD_SET(s_demux_file, &rfds);

	int dump_file = creat("subdump", S_IRUSR | S_IWUSR);

	for (;;) {
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		printf("Waiting in select for 5 seconds\n");
		select_ret = select(s_demux_file + 1, &rfds, NULL, NULL, &tv);
		if (select_ret < 0) {
			perror("select()");
			fprintf(stderr, "Select on s_demux_file returned an error, breaking the infinite loop\n");
			break;
		}
		if (FD_ISSET(s_demux_file, &rfds)) {
			printf("Data available on s_demux_file, copying to file \"subdump\"\n");
			read_len = read(s_demux_file, buf, MAX_READ);
			write_len = write(dump_file, buf, read_len);
			fdatasync(dump_file);
			printf("Copied %d bytes (to %d bytes)\n", read_len, write_len);
		} else {
			printf("One select loop (non-read or timeout)\n");
		}
	}

	close(dump_file);
	closeDVBSubPid();
	return 0;
}
