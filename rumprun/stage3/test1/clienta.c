#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main()
{
	int fd[2], n, i;
	char *buf = "Qello from another unikernel!\n";
	if(my_pipe(fd) < 0)
		perror("pipe");
	printf("USERSPACE: fd: %d - %d\n", fd[0], fd[1]);
	for (i=0; i<17; i++) {
		buf[1] = '0' + i%10;
		n = write(fd[1], buf, 30);
		if(n < 0)
			perror("write");
		printf("USERSPACE: wrote %d bytes\n", n);
		//sleep(1);
	}
	sleep(5);
	//printf("wait toread...\n");
	//n = read(fd[0], buf, 30);
	//if (n < 0)
	//	perror("read:");
	//printf("USERSPACE: read %d bytes\n", n);
	//printf("USERSPACE: Got message: %s\n", buf);
	//fflush(stdout);
	//sleep(5);
	//buf[0] = 'Q';
	//n = write(fd[1], buf, 30);
	//if(n < 0)
	//	perror("write");
	//printf("USERSPACE: wrote %d bytes\n", n);
	//fflush(stdout);
	close(fd[0]);
	close(fd[1]);
	/* needs to wait a bit before halt */
	return 0;
}


